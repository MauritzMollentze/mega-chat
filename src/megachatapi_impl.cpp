/**
 * @file megachatapi_impl.cpp
 * @brief Private implementation of the intermediate layer for the MEGA C++ SDK.
 *
 * (c) 2013-2016 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#define _POSIX_SOURCE
#define _LARGE_FILES

#define _GNU_SOURCE 1
#define _FILE_OFFSET_BITS 64

#define __DARWIN_C_LEVEL 199506L

#define USE_VARARGS
#define PREFER_STDARG

#include <megaapi_impl.h>
#include "megachatapi_impl.h"
#include <base/cservices.h>
#include <base/logger.h>
#include <IGui.h>
#include <chatClient.h>
#include <mega/base64.h>
#include <chatdMsg.h>

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable: 4996) // rapidjson: The std::iterator class template (used as a base class to provide typedefs) is deprecated in C++17. (The <iterator> header is NOT deprecated.)
#endif

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/document.h>

#ifdef _WIN32
#pragma warning(pop)
#endif

#ifndef _WIN32
#include <signal.h>
#endif

#ifndef KARERE_DISABLE_WEBRTC
namespace rtcModule {void globalCleanup(); }
#endif

#define MAX_PUBLICCHAT_MEMBERS_TO_PRIVATE 100
#define MAX_PUBLICCHAT_MEMBERS_FOR_CALL 20

using namespace std;
using namespace megachat;
using namespace mega;
using namespace karere;
using namespace chatd;

LoggerHandler *MegaChatApiImpl::loggerHandler = NULL;

MegaChatApiImpl::MegaChatApiImpl(MegaChatApi *chatApi, MegaApi *megaApi)
{
    init(chatApi, megaApi);
}

MegaChatApiImpl::~MegaChatApiImpl()
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_DELETE);
    requestQueue.push(request);
    waiter->notify();
    thread.join();
    delete request;

    for (auto it = chatPeerListItemHandler.begin(); it != chatPeerListItemHandler.end(); it++)
    {
        delete *it;
    }
    for (auto it = chatGroupListItemHandler.begin(); it != chatGroupListItemHandler.end(); it++)
    {
        delete *it;
    }

    // TODO: destruction of waiter hangs forever or may cause crashes
    //delete waiter;

    // TODO: destruction of network layer may cause hangs on MegaApi's network layer.
    // It may terminate the OpenSSL required by cUrl in SDK, so better to skip it.
    //delete websocketsIO;
}

void MegaChatApiImpl::init(MegaChatApi *chatApi, MegaApi *megaApi)
{
    if (!megaPostMessageToGui)
    {
        megaPostMessageToGui = MegaChatApiImpl::megaApiPostMessage;
    }

    mChatApi = chatApi;
    mMegaApi = megaApi;

    mClient = NULL;
    mTerminating = false;
    waiter = new MegaChatWaiter();
    mWebsocketsIO = new MegaWebsocketsIO(sdkMutex, waiter, megaApi, this);
    reqtag = 0;

    //Start blocking thread
    threadExit = 0;
    thread.start(threadEntryPoint, this);
}

//Entry point for the blocking thread
void *MegaChatApiImpl::threadEntryPoint(void *param)
{
#ifndef _WIN32
    struct sigaction noaction;
    memset(&noaction, 0, sizeof(noaction));
    noaction.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &noaction, 0);
#endif

    MegaChatApiImpl *chatApiImpl = (MegaChatApiImpl *)param;
    chatApiImpl->loop();
    return 0;
}

void MegaChatApiImpl::loop()
{
    sdkMutex.lock();
    while (true)
    {
        sdkMutex.unlock();

        waiter->init(NEVER);
        waiter->wakeupby(mWebsocketsIO, ::mega::Waiter::NEEDEXEC);
        waiter->wait();

        sdkMutex.lock();

        sendPendingEvents();
        sendPendingRequests();

        if (threadExit)
        {
            // There must be only one pending events, at maximum: the logout marshall call to delete the client
            assert(eventQueue.isEmpty() || (eventQueue.size() == 1));
            sendPendingEvents();

            sdkMutex.unlock();
            break;
        }
    }

#ifndef KARERE_DISABLE_WEBRTC
    rtcModule::globalCleanup();
#endif
}

void MegaChatApiImpl::megaApiPostMessage(megaMessage* msg, void* ctx)
{
    MegaChatApiImpl *megaChatApi = (MegaChatApiImpl *)ctx;
    if (megaChatApi)
    {
        megaChatApi->postMessage(msg);
    }
    else
    {
        // For compatibility with the QT example app,
        // there are some marshallCall() without context
        // that don't need to be marshalled using the
        // intermediate layer
        megaProcessMessage(msg);
    }
}

void MegaChatApiImpl::postMessage(megaMessage* msg)
{
    eventQueue.push(msg);
    waiter->notify();
}

void MegaChatApiImpl::sendPendingRequests()
{
    MegaChatRequestPrivate *request;
    int errorCode = MegaChatError::ERROR_OK;
    int nextTag = 0;

    while((request = requestQueue.pop()))
    {
        nextTag = ++reqtag;
        request->setTag(nextTag);
        requestMap[nextTag]=request;
        errorCode = MegaChatError::ERROR_OK;

        fireOnChatRequestStart(request);

        if (!mClient && request->getType() != MegaChatRequest::TYPE_DELETE)
        {
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_ACCESS);
            API_LOG_WARNING("Chat engine not initialized yet, cannot process the request");
            fireOnChatRequestFinish(request, megaChatError);
            continue;
        }

        if (mTerminating && request->getType() != MegaChatRequest::TYPE_DELETE)
        {
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_ACCESS);
            API_LOG_WARNING("Chat engine is terminated, cannot process the request");
            fireOnChatRequestFinish(request, megaChatError);
            continue;
        }

        switch (request->getType())
        {
        case MegaChatRequest::TYPE_CONNECT:
        {
            bool isInBackground = request->getFlag();

            mClient->connect(isInBackground)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& e)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(e.msg(), e.code(), e.type());
                fireOnChatRequestFinish(request, megaChatError);
            });

            break;
        }
        case MegaChatRequest::TYPE_DISCONNECT:
        {
            // mClient->disconnect();   --> obsolete
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_ACCESS);
            fireOnChatRequestFinish(request, megaChatError);

            break;
        }
        case MegaChatRequest::TYPE_RETRY_PENDING_CONNECTIONS:
        {
            bool disconnect = request->getFlag();
            bool refreshURLs = (bool)(request->getParamType() == 1);
            mClient->retryPendingConnections(disconnect, refreshURLs);

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_SEND_TYPING_NOTIF:
        {
            MegaChatHandle chatid = request->getChatHandle();
            ChatRoom *chatroom = findChatRoom(chatid);
            if (chatroom)
            {
                if (request->getFlag())
                {
                    chatroom->sendTypingNotification();
                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                }
                else
                {
                    chatroom->sendStopTypingNotification();
                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                }
            }
            else
            {
                errorCode = MegaChatError::ERROR_ARGS;
            }
            break;
        }
        case MegaChatRequest::TYPE_SIGNAL_ACTIVITY:
        {
            mClient->presenced().signalActivity();
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);

            break;
        }
        case MegaChatRequest::TYPE_SET_PRESENCE_AUTOAWAY:
        {
            int64_t timeout = request->getNumber();
            bool enable = request->getFlag();

            if (timeout > presenced::Config::kMaxAutoawayTimeout)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            mClient->presenced().setAutoaway(enable, timeout);

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);

            break;
        }

        case MegaChatRequest::TYPE_SET_PRESENCE_PERSIST:
        {
            bool enable = request->getFlag();

            mClient->presenced().setPersist(enable);

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);

            break;
        }
        case MegaChatRequest::TYPE_SET_LAST_GREEN_VISIBLE:
        {
            bool enable = request->getFlag();
            mClient->presenced().setLastGreenVisible(enable);
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);

            break;
        }
        case MegaChatRequest::TYPE_LAST_GREEN:
        {
            MegaChatHandle userid = request->getUserHandle();
            mClient->presenced().requestLastGreen(userid);
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);

            break;
        }
        case MegaChatRequest::TYPE_LOGOUT:
        {
            bool deleteDb = request->getFlag();
            cleanChatHandlers();
            mTerminating = true;
            mClient->terminate(deleteDb);

            API_LOG_INFO("Chat engine is logged out!");
            marshallCall([request, this]() //post destruction asynchronously so that all pending messages get processed before that
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);

                delete mClient;
                mClient = NULL;
                mTerminating = false;
            }, this);

            break;
        }
        case MegaChatRequest::TYPE_DELETE:
        {
            if (mClient && !mTerminating)
            {
                cleanChatHandlers();
                mClient->terminate();
                API_LOG_INFO("Chat engine closed!");

                delete mClient;
                mClient = NULL;
            }

            threadExit = 1;
            break;
        }
        case MegaChatRequest::TYPE_SET_ONLINE_STATUS:
        {
            int status = request->getNumber();
            if (status < MegaChatApi::STATUS_OFFLINE || status > MegaChatApi::STATUS_BUSY)
            {
                fireOnChatRequestFinish(request, new MegaChatErrorPrivate("Invalid online status", MegaChatError::ERROR_ARGS));
                break;
            }

            mClient->setPresence(request->getNumber())
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error setting online status: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }

        case MegaChatRequest::TYPE_CREATE_CHATROOM:
        {
            MegaChatPeerList *peersList = request->getMegaChatPeerList();
            if (!peersList)   // force to provide a list, even without participants
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            bool isMeeting = request->getNumber();
            bool publicChat = request->getPrivilege();
            bool group = request->getFlag();
            const userpriv_vector *userpriv = ((MegaChatPeerListPrivate*)peersList)->getList();
            if (!userpriv || (!group && publicChat))
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (!group && peersList->size() == 0)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (!group && peersList->size() > 1)
            {
                group = true;
                request->setFlag(group);
                API_LOG_INFO("Forcing group chat due to more than 2 participants");
            }

            if (group)
            {
                const char *title = request->getText();
                vector<std::pair<handle, Priv>> peers;
                for (unsigned int i = 0; i < userpriv->size(); i++)
                {
                    peers.push_back(std::make_pair(userpriv->at(i).first, (Priv) userpriv->at(i).second));
                }

                if (title)  // not mandatory
                {
                    string strTitle(title);
                    strTitle = strTitle.substr(0, 30);
                    request->setText(strTitle.c_str()); // update, in case it's been truncated
                    title = request->getText();
                }

                mClient->createGroupChat(peers, publicChat, isMeeting, title)
                .then([request, this](Id chatid)
                {
                    request->setChatHandle(chatid);
                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);

                })
                .fail([request,this](const ::promise::Error& err)
                {
                    API_LOG_ERROR("Error creating group chat: %s", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });
            }
            else    // 1on1 chat
            {
                if (peersList->getPeerHandle(0) == mClient->myHandle())
                {
                    // can't create a 1on1 chat with own user.
                    errorCode = MegaChatError::ERROR_NOENT;
                    break;
                }
                ContactList::iterator it = mClient->mContactList->find(peersList->getPeerHandle(0));
                if (it == mClient->mContactList->end())
                {
                    // contact not found
                    errorCode = MegaChatError::ERROR_ACCESS;
                    break;
                }
                if (it->second->chatRoom())
                {
                    // chat already exists
                    request->setChatHandle(it->second->chatRoom()->chatid());
                    errorCode = MegaChatError::ERROR_OK;
                    break;
                }
                it->second->createChatRoom()
                .then([request,this](ChatRoom* room)
                {
                    request->setChatHandle(room->chatid());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                })
                .fail([request,this](const ::promise::Error& err)
                {
                    API_LOG_ERROR("Error creating 1on1 chat: %s", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });
            }
            break;
        }
        case MegaChatRequest::TYPE_INVITE_TO_CHATROOM:
        {
            handle chatid = request->getChatHandle();
            handle uh = request->getUserHandle();
            Priv privilege = (Priv) request->getPrivilege();

            if (chatid == MEGACHAT_INVALID_HANDLE || uh == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (!chatroom->isGroup())   // invite only for group chats
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }
            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            ((GroupChatRoom *)chatroom)->invite(uh, privilege)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error adding user to group chat: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }

        case MegaChatRequest::TYPE_AUTOJOIN_PUBLIC_CHAT:
        {
            handle chatid = request->getChatHandle();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (!chatroom->isGroup()
                || !chatroom->publicChat()
                || (!chatroom->previewMode() && chatroom->isActive())) // cannot autojoin an active chat if it's not in preview mode
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            handle phTmp = request->getUserHandle();
            handle ph;
            if (phTmp != MEGACHAT_INVALID_HANDLE)
            // rejoin inactive/left public chat
            {
                ph = phTmp;
            }
            else  // join chat in preview (previously loaded)
            {
                ph = chatroom->getPublicHandle();
            }

            if (ph == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ((GroupChatRoom *)chatroom)->autojoinPublicChat(ph)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error joining user to public group chat: %s", err.what());
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });

            break;
        }

        case MegaChatRequest::TYPE_UPDATE_PEER_PERMISSIONS:
        {
            handle chatid = request->getChatHandle();
            handle uh = request->getUserHandle();
            Priv privilege = (Priv) request->getPrivilege();

            if (chatid == MEGACHAT_INVALID_HANDLE || uh == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            ((GroupChatRoom *)chatroom)->setPrivilege(uh, privilege)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error updating peer privileges: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_REMOVE_FROM_CHATROOM:
        {
            handle chatid = request->getChatHandle();
            handle uh = request->getUserHandle();

            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (!chatroom->isGroup())   // only for group chats can be left
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if ( uh == MEGACHAT_INVALID_HANDLE || uh == mClient->myHandle())
            {
                ((GroupChatRoom *)chatroom)->leave()
                .then([request, this]()
                {
                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                })
                .fail([request, this](const ::promise::Error& err)
                {
                    API_LOG_ERROR("Error leaving chat: %s", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });
            }
            else
            {
                if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
                {
                        errorCode = MegaChatError::ERROR_ACCESS;
                        break;
                }

                ((GroupChatRoom *)chatroom)->excludeMember(uh)
                .then([request, this]()
                {
                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                })
                .fail([request, this](const ::promise::Error& err)
                {
                    API_LOG_ERROR("Error removing peer from chat: %s", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });
            }
            break;
        }
        case MegaChatRequest::TYPE_TRUNCATE_HISTORY:
        {
            handle chatid = request->getChatHandle();

            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            handle messageid = request->getUserHandle();
            if (messageid == MEGACHAT_INVALID_HANDLE)   // clear the full history, from current message
            {
                if (chatroom->chat().empty())
                {
                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                    break;
                }

                messageid = chatroom->chat().at(chatroom->chat().highnum()).id().val;
            }

            chatroom->truncateHistory(messageid)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error truncating chat history: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_EDIT_CHATROOM_NAME:
        {
            handle chatid = request->getChatHandle();
            const char *title = request->getText();
            if (chatid == MEGACHAT_INVALID_HANDLE || title == NULL || !strlen(title))
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (!chatroom->isGroup())   // only for group chats have a title
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (chatroom->ownPriv() != (Priv) MegaChatPeerList::PRIV_MODERATOR)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            string strTitle(title);
            strTitle = strTitle.substr(0, 30);
            request->setText(strTitle.c_str()); // update, in case it's been truncated

            ((GroupChatRoom *)chatroom)->setTitle(strTitle)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error editing chat title: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_LOAD_PREVIEW:
        {
            string parsedLink = request->getLink();
            if (parsedLink.empty())
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            //link format: https://mega.nz/chat/<public-handle>#<chat-key>
            string separator = "chat/";
            size_t pos = parsedLink.find(separator);
            if (pos == string::npos)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }
            pos += separator.length();

            //We get the substring "<public-handle>#<chat-key>"
            parsedLink = parsedLink.substr(pos);
            separator = "#";
            pos = parsedLink.find(separator);

            if (pos == string::npos
                || pos != 8
                || (parsedLink.size() - pos - 1) < 22)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            //Parse public handle (First 8 Bytes)
            string phstr = parsedLink.substr(0, pos);   // 6 bytes in binary, 8 in B64url
            MegaChatHandle ph = 0;
            Base64::atob(phstr.data(), (::mega::byte*)&ph, MegaClient::CHATLINKHANDLE);

            //Parse unified key (Last 16 Bytes)
            string unifiedKey; // it's 16 bytes in binary, 22 in B64url
            string keystr = parsedLink.substr(pos + 1, 22);
            Base64::atob(keystr, unifiedKey);

            //Check that ph and uk have right size
            if (ISUNDEF(ph) || unifiedKey.size() != 16)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            mClient->openChatPreview(ph)
            .then([request, this, unifiedKey, ph](ReqResult result)
            {
                assert(result);

                std::string encTitle = result->getText() ? result->getText() : "";
                assert(!encTitle.empty());

                uint64_t chatId = result->getParentHandle();

                mClient->decryptChatTitle(chatId, unifiedKey, encTitle, ph)
                .then([request, this, unifiedKey, result, chatId](std::string decryptedTitle)
                {
                   bool createChat = request->getFlag();
                   int numPeers = result->getNumDetails();
                   request->setChatHandle(chatId);
                   request->setNumber(numPeers);
                   request->setText(decryptedTitle.c_str());
                   if (result->getMegaHandleList())
                   {
                       request->setMegaHandleList(result->getMegaHandleList());
                   }

                   bool meeting = result->getFlag();
                   if (meeting)
                   {
                       request->setParamType(1);
                   }

                   //Check chat link
                   if (!createChat)
                   {
                       MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                       fireOnChatRequestFinish(request, megaChatError);
                   }
                   else //Load chat link
                   {
                       Id ph = result->getNodeHandle();
                       request->setUserHandle(ph.val);

                       GroupChatRoom *room = (GroupChatRoom*) findChatRoom(chatId);
                       if (room)
                       {
                           if (room->isActive()
                              || (!room->isActive() && !room->previewMode()))
                           {
                               MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_EXIST);
                               fireOnChatRequestFinish(request, megaChatError);
                           }
                           else
                           {
                               MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                               room->enablePreview(ph);
                               fireOnChatRequestFinish(request, megaChatError);
                           }
                       }
                       else
                       {
                           std::string url = result->getLink() ? result->getLink() : "";
                           int shard = result->getAccess();
                           std::shared_ptr<std::string> key = std::make_shared<std::string>(unifiedKey);
                           uint32_t ts = result->getNumber();

                           mClient->createPublicChatRoom(chatId, ph.val, shard, decryptedTitle, key, url, ts, meeting);
                           MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                           fireOnChatRequestFinish(request, megaChatError);
                       }
                   }
                })
                .fail([request, this](const ::promise::Error& err)
                {
                    API_LOG_ERROR("Error decrypting chat title: %s", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error loading chat link: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_SET_PRIVATE_MODE:
        {
            MegaChatHandle chatid = request->getChatHandle();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *room = findChatRoom(chatid);
            if (!room)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (!room->isGroup() || !room->publicChat())
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (room->numMembers() > MAX_PUBLICCHAT_MEMBERS_TO_PRIVATE)
            {
                errorCode = MegaChatError::ERROR_TOOMANY;
                break;
            }

            if (room->ownPriv() != chatd::PRIV_OPER)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            mClient->setPublicChatToPrivate(chatid)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });

            break;
        }
        case MegaChatRequest::TYPE_CHAT_LINK_HANDLE:
        {
            MegaChatHandle chatid = request->getChatHandle();
            bool del = request->getFlag();
            bool createifmissing = request->getNumRetry();

            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            GroupChatRoom *room = (GroupChatRoom *) findChatRoom(chatid);
            if (!room)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (del && createifmissing)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (!room->publicChat() || !room->isGroup())
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            // anyone can retrieve an existing link, but only operator can create/delete it
            int ownPriv = room->ownPriv();
            if ((ownPriv == Priv::PRIV_NOTPRESENT)
                 || ((del || createifmissing) && ownPriv != Priv::PRIV_OPER))
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (!del && createifmissing && !room->hasTitle())
            {
                API_LOG_DEBUG("Cannot create chat-links on chatrooms without title");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            ::promise::Promise<uint64_t> pms;
            if (del)
            {
                pms = mClient->deleteChatLink(chatid);
            }
            else    // query or create
            {
                pms = mClient->getPublicHandle(chatid, createifmissing);
            }

            pms.then([request, del, room, this] (uint64_t ph)
            {
                if (del)
                {
                    return fireOnChatRequestFinish(request, new MegaChatErrorPrivate(MegaChatError::ERROR_OK));
                }
                else if (ph == Id::inval())
                {
                    API_LOG_ERROR("Unexpected invalid public handle for query/create chat-link");
                    return fireOnChatRequestFinish(request, new MegaChatErrorPrivate(MegaChatError::ERROR_NOENT));
                }

                room->unifiedKey()
                .then([request, this, ph] (shared_ptr<string> unifiedKey)
                {
                    MegaChatErrorPrivate *megaChatError;
                    if (!unifiedKey || unifiedKey->size() != 16)
                    {
                        API_LOG_ERROR("Invalid unified key after decryption");
                        megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_NOENT);
                    }
                    else    // prepare the link with ph + unifiedKey
                    {
                        string unifiedKeyB64;
                        Base64::btoa(*unifiedKey, unifiedKeyB64);

                        string phBin((const char*)&ph, MegaClient::CHATLINKHANDLE);
                        string phB64;
                        Base64::btoa(phBin, phB64);

                        string link = "https://mega.nz/chat/" + phB64 + "#" + unifiedKeyB64;
                        request->setText(link.c_str());

                        megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    }
                    fireOnChatRequestFinish(request, megaChatError);
                })
                .fail([request, this] (const ::promise::Error &err)
                {
                    API_LOG_ERROR("Failed to decrypt unified key: %s", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });

            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Failed to query/create/delete chat-link: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_GET_FIRSTNAME:
        {
            // if the app requested user attributes too early (ie. init with sid but without cache),
            // the cache will not be ready yet. It needs to wait for fetchnodes to complete.
            if (!mClient->isUserAttrCacheReady())
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            MegaChatHandle uh = request->getUserHandle();
            const char* publicHandle = request->getLink();
            MegaChatHandle ph = publicHandle ? karere::Id(publicHandle, strlen(publicHandle)).val : MEGACHAT_INVALID_HANDLE;

            mClient->userAttrCache().getAttr(uh, MegaApi::USER_ATTR_FIRSTNAME, ph)
            .then([request, this](Buffer *data)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                string firstname = string(data->buf(), data->dataSize());
                request->setText(firstname.c_str());
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error getting user firstname: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_GET_LASTNAME:
        {
            // if the app requested user attributes too early (ie. init with sid but without cache),
            // the cache will not be ready yet. It needs to wait for fetchnodes to complete.
            if (!mClient->isUserAttrCacheReady())
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            MegaChatHandle uh = request->getUserHandle();
            const char* publicHandle = request->getLink();
            MegaChatHandle ph = publicHandle ? karere::Id(publicHandle, strlen(publicHandle)).val : MEGACHAT_INVALID_HANDLE;

            mClient->userAttrCache().getAttr(uh, MegaApi::USER_ATTR_LASTNAME, ph)
            .then([request, this](Buffer *data)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                string lastname = string(data->buf(), data->dataSize());
                request->setText(lastname.c_str());
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error getting user lastname: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_GET_EMAIL:
        {
            // if the app requested user attributes too early (ie. init with sid but without cache),
            // the cache will not be ready yet. It needs to wait for fetchnodes to complete.
            if (!mClient->isUserAttrCacheReady())
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            MegaChatHandle uh = request->getUserHandle();
            mClient->userAttrCache().getAttr(uh, karere::USER_ATTR_EMAIL)
            .then([request, this](Buffer *data)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                string email = string(data->buf(), data->dataSize());
                request->setText(email.c_str());
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error getting user email: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_ATTACH_NODE_MESSAGE:
        {
            handle chatid = request->getChatHandle();
            MegaNodeList *nodeList = request->getMegaNodeList();
            handle h = request->getUserHandle();
            bool isVoiceMessage = (request->getParamType() == 1);
            if (chatid == MEGACHAT_INVALID_HANDLE
                    || ((!nodeList || !nodeList->size()) && (h == MEGACHAT_INVALID_HANDLE))
                    || (isVoiceMessage && h == MEGACHAT_INVALID_HANDLE))
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            // if only one node, prepare a list with a single element and update request
            MegaNodeList *nodeListAux = NULL;
            if (h != MEGACHAT_INVALID_HANDLE)
            {
                MegaNode *megaNode = mMegaApi->getNodeByHandle(h);
                if (!megaNode)
                {
                    errorCode = MegaChatError::ERROR_NOENT;
                    break;
                }

                nodeListAux = MegaNodeList::createInstance();
                nodeListAux->addNode(megaNode);
                request->setMegaNodeList(nodeListAux);
                nodeList = request->getMegaNodeList();

                delete megaNode;
                delete nodeListAux;
            }

            uint8_t msgType = Message::kMsgInvalid;
            switch (request->getParamType())
            {
                case 0: // regular attachment
                    msgType = Message::kMsgAttachment;
                    break;

                case 1:  // voice-message
                    msgType = Message::kMsgVoiceClip;
                    break;
            }
            if (msgType == Message::kMsgInvalid)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            string buffer = JSonUtils::generateAttachNodeJSon(nodeList, msgType);
            if (buffer.empty())
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            chatroom->requesGrantAccessToNodes(nodeList)
            .then([this, request, buffer, msgType]()
            {
                int errorCode = MegaChatError::ERROR_ARGS;
                MegaChatMessage *msg = sendMessage(request->getChatHandle(), buffer.c_str(), buffer.size(), msgType);
                if (msg)
                {
                    request->setMegaChatMessage(msg);
                    errorCode = MegaChatError::ERROR_OK;
                }

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(errorCode);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([this, request, buffer, msgType](const ::promise::Error& err)
            {
                MegaChatErrorPrivate *megaChatError = NULL;
                if (err.code() == MegaChatError::ERROR_EXIST)
                {
                    API_LOG_WARNING("Already granted access to this node previously");

                    int errorCode = MegaChatError::ERROR_ARGS;
                    MegaChatMessage *msg = sendMessage(request->getChatHandle(), buffer.c_str(), buffer.size(), msgType);
                    if (msg)
                    {
                        request->setMegaChatMessage(msg);
                        errorCode = MegaChatError::ERROR_OK;
                    }

                    megaChatError = new MegaChatErrorPrivate(errorCode);
                }
                else
                {
                    megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    API_LOG_ERROR("Failed to grant access to some nodes");
                }

                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_REVOKE_NODE_MESSAGE:
        {
            MegaChatHandle chatid = request->getChatHandle();
            MegaNode *node = mMegaApi->getNodeByHandle(request->getUserHandle());
            if (chatid == MEGACHAT_INVALID_HANDLE || !node)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ::promise::Promise<void> promise = chatroom->requestRevokeAccessToNode(node);
            delete node;

            ::promise::when(promise)
            .then([this, request]()
            {
                std::string buf = Id(request->getUserHandle()).toString();
                buf.insert(buf.begin(), Message::kMsgRevokeAttachment - Message::kMsgOffset);
                buf.insert(buf.begin(), 0x0);

                MegaChatMessage *megaMsg = sendMessage(request->getChatHandle(), buf.c_str(), buf.length());
                request->setMegaChatMessage(megaMsg);

                int errorCode = MegaChatError::ERROR_OK;
                if (!megaMsg)
                {
                    errorCode = MegaChatError::ERROR_ARGS;
                }

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(errorCode);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([this, request](const ::promise::Error& err)
            {
                API_LOG_ERROR("Failed to revoke access to attached node (%d)", request->getUserHandle());
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_SET_BACKGROUND_STATUS:
        {
            bool background = request->getFlag();
            mClient->notifyUserStatus(background)
            .then([this, request]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([this, request](const ::promise::Error& err)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_PUSH_RECEIVED:
        {
            MegaChatHandle chatid = request->getChatHandle();
            ChatRoom *room = findChatRoom(chatid);
            bool wasArchived = (room && room->isArchived());

            mClient->pushReceived(chatid)
            .then([this, request, wasArchived]()
            {
                int type = request->getParamType();
                if (type == 0)  // Android
                {
                    // for Android, we prepare a list of msgids for every chatid that are candidates for
                    // notifications. Android doesn't really know why they received a push, so the previous
                    // notifications are cleanup and the new set of messages are notified

                    MegaHandleList *chatids = MegaHandleList::createInstance();

                    // for each chatroom, load all unread messages)
                    for (auto it = mClient->chats->begin(); it != mClient->chats->end(); it++)
                    {
                        MegaChatHandle chatid = it->first;
                        // don't want to generate notifications for archived chats or chats with notifications disabled
                        if (it->second->isArchived() || !mMegaApi->isChatNotifiable(chatid))
                            continue;

                        MegaHandleList *msgids = MegaHandleList::createInstance();

                        const Chat &chat = it->second->chat();
                        Idx lastSeenIdx = chat.lastSeenIdx();

                        // first msg to consider: last-seen if loaded in memory. Otherwise, the oldest loaded msg
                        Idx first = chat.lownum();
                        if (lastSeenIdx != CHATD_IDX_INVALID        // message is known locally
                                && chat.findOrNull(lastSeenIdx))    // message is loaded in RAM
                        {
                            first = lastSeenIdx + 1;
                        }
                        Idx last = chat.highnum();
                        int maxCount = 6;   // do not notify more than 6 messages per chat
                        for (Idx i = last; (i >= first && maxCount > 0); i--)
                        {
                            auto& msg = chat.at(i);
                            if (msg.isValidUnread(mClient->myHandle()))
                            {
                                maxCount--;
                                msgids->addMegaHandle(msg.id());
                            }
                        }

                        if (msgids->size())
                        {
                            chatids->addMegaHandle(chatid);
                            request->setMegaHandleListByChat(chatid, msgids);
                        }

                        delete msgids;
                    }

                    request->setMegaHandleList(chatids);    // always a valid list, even if empty
                    delete chatids;
                }
                else    // iOS
                {
                    MegaChatHandle chatid = request->getChatHandle();
                    ChatRoom *room = findChatRoom(chatid);
                    if (!room)
                    {
                        mMegaApi->sendEvent(99006, "iOS PUSH received for non-existing chatid");

                        MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_NOENT);
                        fireOnChatRequestFinish(request, megaChatError);
                        return;
                    }
                    else if (wasArchived && room->isArchived())    // don't want to generate notifications for archived chats
                    {
                        mMegaApi->sendEvent(99009, "PUSH received for archived chatid (and still archived)");

                        // since a PUSH could be received before the actionpacket updating flags (
                        MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_ACCESS);
                        fireOnChatRequestFinish(request, megaChatError);
                        return;
                    }
                }

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([this, request](const ::promise::Error& err)
            {
                API_LOG_ERROR("Failed to retrieve current state");
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
#ifndef KARERE_DISABLE_WEBRTC
        case MegaChatRequest::TYPE_START_CHAT_CALL:
        {
            if (!mClient->rtc)
            {
                API_LOG_ERROR("Start call - WebRTC is not initialized");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            MegaChatHandle chatid = request->getChatHandle();
            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                API_LOG_ERROR("Start call - Chatroom has not been found");
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (chatroom->publicChat() && chatroom->numMembers() > MAX_PUBLICCHAT_MEMBERS_FOR_CALL)
            {
                API_LOG_ERROR("Start call - the public chat has too many participants");
                errorCode = MegaChatError::ERROR_TOOMANY;
                break;
            }

            if (!chatroom->isGroup())
            {
                uint64_t uh = ((PeerChatRoom*)chatroom)->peer();
                Contact *contact = mClient->mContactList->contactFromUserId(uh);
                if (!contact || contact->visibility() != ::mega::MegaUser::VISIBILITY_VISIBLE)
                {
                    API_LOG_ERROR("Start call - Refusing start a call with a non active contact");
                    errorCode = MegaChatError::ERROR_ACCESS;
                    break;
                }
            }
            else if (chatroom->ownPriv() <= Priv::PRIV_RDONLY)
            {
                API_LOG_ERROR("Start call - Refusing start a call withouth enough privileges");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (chatroom->previewMode())
            {
                API_LOG_ERROR("Start call - Chatroom is in preview mode");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (chatroom->chatdOnlineState() != kChatStateOnline)
            {
                API_LOG_ERROR("Start call - chatroom isn't in online state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            bool enableVideo = request->getFlag();
            bool enableAudio = request->getParamType();
            karere::AvFlags avFlags(enableAudio, enableVideo);
            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
               ::promise::Promise<std::shared_ptr<std::string>> pms;
               if (chatroom->publicChat())
               {
                   pms = static_cast<GroupChatRoom *>(chatroom)->unifiedKey();
               }
               else
               {
                   pms.resolve(std::make_shared<string>());
               }

               bool isGroup = chatroom->isGroup();
               pms.then([request, this, chatid, avFlags, isGroup] (shared_ptr<string> unifiedKey)
               {
                   mClient->rtc->startCall(chatid, avFlags, isGroup, unifiedKey)
                   .then([request, this]()
                   {
                       MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                       fireOnChatRequestFinish(request, megaChatError);
                   })
                   .fail([request, this](const ::promise::Error& err)
                   {
                       API_LOG_ERROR("Error Starting a chat call: %s", err.what());

                       MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                       fireOnChatRequestFinish(request, megaChatError);
                   });
               })
               .fail([request, this] (const ::promise::Error &err)
               {
                   API_LOG_ERROR("Failed to decrypt unified key: %s", err.what());
                   MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                   fireOnChatRequestFinish(request, megaChatError);
               });
            }
            else if (!call->participate())
            {
                if (call->getParticipants().size() > rtcModule::kMaxCallReceivers)
                {
                    API_LOG_ERROR("Start call - There are too many participants in the call");
                    errorCode = MegaChatError::ERROR_TOOMANY;
                    break;
                }

                call->join(avFlags)
                .then([request, this]()
                {
                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                    fireOnChatRequestFinish(request, megaChatError);
                })
                .fail([request, this](const ::promise::Error& err)
                {
                    API_LOG_ERROR("Error Joining a chat call: %s", err.what());

                    MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                    fireOnChatRequestFinish(request, megaChatError);
                });
            }
            else
            {
                // only groupchats allow to join the call in multiple clients, in 1on1 it's not allowed
                API_LOG_ERROR("A call exists in this chatroom and we already participate or it's not a groupchat");
                errorCode = MegaChatError::ERROR_EXIST;
                break;
            }

            break;
        }
        case MegaChatRequest::TYPE_ANSWER_CHAT_CALL:
        {
            MegaChatHandle chatid = request->getChatHandle();

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                API_LOG_ERROR("Answer call - Chatroom has not been found");
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (chatroom->publicChat() && chatroom->numMembers() > MAX_PUBLICCHAT_MEMBERS_FOR_CALL)
            {
                API_LOG_ERROR("Answer call - the public chat has too many participants");
                errorCode = MegaChatError::ERROR_TOOMANY;
                break;
            }

            if (!chatroom->chat().connection().clientId())
            {
                API_LOG_ERROR("Answer call - Refusing answer a call, clientid no yet assigned by shard: %d", chatroom->chat().connection().shardNo());
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (chatroom->chatdOnlineState() != kChatStateOnline)
            {
                API_LOG_ERROR("Answer call - chatroom isn't in online state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("Answer call - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (call->getParticipants().size() > rtcModule::kMaxCallReceivers)
            {
                API_LOG_ERROR("Answer call - There are too many participants in the call");
                errorCode = MegaChatError::ERROR_TOOMANY;
                break;
            }

            if (call->participate())
            {
                API_LOG_ERROR("Answer call - You already participate");
                errorCode = MegaChatError::ERROR_EXIST;
                break;
            }

            bool enableVideo = request->getFlag();
            bool enableAudio = request->getParamType();
            karere::AvFlags avFlags(enableAudio, enableVideo);
            call->join(avFlags)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error Joining a chat call: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });

            break;
        }
        case MegaChatRequest::TYPE_HANG_CHAT_CALL:
        {
            if (!mClient->rtc)
            {
                API_LOG_ERROR("Hang up call - WebRTC is not initialized");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            MegaChatHandle callid = request->getChatHandle();
            if (callid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("Hang up call - invalid callid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall* call = mClient->rtc->findCall(callid);

            if (!call)
            {
                API_LOG_ERROR("Hang up call - There is not any call with that callid");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            ChatRoom *chatroom = findChatRoom(call->getChatid());
            if (!chatroom)
            {
                API_LOG_ERROR("Hang up call- Chatroom has not been found");
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            bool moderator = chatroom->chat().getOwnprivilege() == PRIV_OPER;
            bool endCall = request->getFlag();
            if (endCall && !moderator)
            {
                API_LOG_ERROR("End call withouth enough privileges");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (endCall)
            {
                // TODO remove this block when we add support for endCall, and re-check conditions
                API_LOG_ERROR("End call not supported yet");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ::promise::Promise<void> pms = endCall
                    ? call->endCall()   // end call
                    : call->hangup();   // hang up

            pms.then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error hang up a chat call: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });

            break;
        }
        case MegaChatRequest::TYPE_DISABLE_AUDIO_VIDEO_CALL:
        {
            MegaChatHandle chatid = request->getChatHandle();
            bool enable = request->getFlag();
            int operationType = request->getParamType();
            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("Disable AV flags  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (operationType != MegaChatRequest::AUDIO && operationType != MegaChatRequest::VIDEO)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            karere::AvFlags currentFlags = call->getLocalAvFlags();
            karere::AvFlags requestedFlags = currentFlags;
            if (operationType == MegaChatRequest::AUDIO)
            {
                if (enable)
                {
                    requestedFlags.add(karere::AvFlags::kAudio);
                }
                else
                {
                    requestedFlags.remove(karere::AvFlags::kAudio);
                }
            }
            else // (operationType == MegaChatRequest::VIDEO)
            {
                if (enable)
                {
                    requestedFlags.add(karere::AvFlags::kCamera);
                }
                else
                {
                    requestedFlags.remove(karere::AvFlags::kCamera);
                }
            }

            if (!call->participate())
            {
                API_LOG_ERROR("Disable audio video - You don't participate in the call");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            call->updateAndSendLocalAvFlags(requestedFlags);

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_SET_CALL_ON_HOLD:
        {
            MegaChatHandle chatid = request->getChatHandle();
            bool onHold = request->getFlag();

            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("Set call on hold  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (call->getState() != rtcModule::CallState::kStateInProgress)
            {
                API_LOG_ERROR("The call can't be set onHold until call is in-progres");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            karere::AvFlags currentFlags = call->getLocalAvFlags();
            if (onHold == currentFlags.isOnHold())
            {
                API_LOG_ERROR("Set call on hold - Call is on hold and try to set on hold or conversely");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }
            currentFlags.setOnHold(onHold);
            call->updateAndSendLocalAvFlags(currentFlags);
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }

        case MegaChatRequest::TYPE_CHANGE_VIDEO_STREAM:
        {
            if (!mClient->rtc)
            {
                API_LOG_ERROR("Change video streaming source - WebRTC is not initialized");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            const char *deviceName = request->getText();
            if (!deviceName || !mClient->rtc->selectVideoInDevice(deviceName))
            {
                API_LOG_ERROR("Change video streaming source - device doesn't exist");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
#endif
        case MegaChatRequest::TYPE_ARCHIVE_CHATROOM:
        {
            handle chatid = request->getChatHandle();
            bool archive = request->getFlag();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            chatroom->archiveChat(archive)
            .then([request, this]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error archiving chat: %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_GET_PEER_ATTRIBUTES:
        {
            // if the app requested user attributes too early (ie. init with sid but without cache),
            // the cache will not be ready yet. It needs to wait for fetchnodes to complete.
            if (!mClient->isUserAttrCacheReady())
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            handle chatid = request->getChatHandle();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            MegaHandleList *handleList = request->getMegaHandleList();
            if (!handleList)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }
            bool isMember = true;
            for (unsigned int i = 0; i < handleList->size(); i++)
            {
                MegaChatHandle peerid = handleList->get(i);
                if (!chatroom->isMember(peerid))
                {
                    API_LOG_ERROR("Error %s is not a chat member of chatroom (%s)", Id(peerid).toString().c_str(), Id(chatid).toString().c_str());
                    isMember = false;
                    break;
                }
            }
            if (!isMember)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }


            std::vector<::promise::Promise<void>> promises;
            for (unsigned int i = 0; i < handleList->size(); i++)
            {
                promises.push_back(chatroom->chat().requestUserAttributes(handleList->get(i)));
            }

            ::promise::when(promises)
            .then([this, request]()
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& e)
            {
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(e.msg(), e.code(), e.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_SET_RETENTION_TIME:
        {
            MegaChatHandle chatid = request->getChatHandle();
            unsigned period = static_cast <unsigned>(request->getNumber());

            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }
            if (chatroom->ownPriv() != static_cast<Priv>(MegaChatPeerList::PRIV_MODERATOR))
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            auto wptr = mClient->weakHandle();
            chatroom->setChatRetentionTime(period)
            .then([request, wptr, this]()
            {
                wptr.throwIfDeleted();
                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
                fireOnChatRequestFinish(request, megaChatError);
            })
            .fail([request, this](const ::promise::Error& err)
            {
                API_LOG_ERROR("Error setting retention time : %s", err.what());

                MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(err.msg(), err.code(), err.type());
                fireOnChatRequestFinish(request, megaChatError);
            });
            break;
        }
        case MegaChatRequest::TYPE_MANAGE_REACTION:
        {
            const char *reaction = request->getText();
            if (!reaction)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            MegaChatHandle chatid = request->getChatHandle();
            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (chatroom->ownPriv() < static_cast<chatd::Priv>(MegaChatPeerList::PRIV_STANDARD))
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            MegaChatHandle msgid = request->getUserHandle();
            Chat &chat = chatroom->chat();
            Idx index = chat.msgIndexFromId(msgid);
            if (index == CHATD_IDX_INVALID)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            const Message &msg = chat.at(index);
            if (msg.isManagementMessage())
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            int pendingStatus = chat.getPendingReactionStatus(reaction, msg.id());
            bool hasReacted = msg.hasReacted(reaction, mClient->myHandle());
            if (request->getFlag())
            {
                // check if max number of reactions has been reached
                int res = msg.allowReact(mClient->myHandle(), reaction);
                if (res != 0)
                {
                    request->setNumber(res);
                    errorCode = MegaChatError::ERROR_TOOMANY;
                    break;
                }

                if ((hasReacted && pendingStatus != OP_DELREACTION)
                    || (!hasReacted && pendingStatus == OP_ADDREACTION))
                {
                    // If the reaction exists and there's not a pending DELREACTION
                    // or the reaction doesn't exists and a ADDREACTION is pending
                    errorCode = MegaChatError::ERROR_EXIST;
                    break;
                }
                else
                {
                    chat.manageReaction(msg, reaction, OP_ADDREACTION);
                }
            }
            else
            {
                if ((!hasReacted && pendingStatus != OP_ADDREACTION)
                    || (hasReacted && pendingStatus == OP_DELREACTION))
                {
                    // If the reaction doesn't exist and there's not a pending ADDREACTION
                    // or reaction exists and a DELREACTION is pending
                    errorCode = MegaChatError::ERROR_EXIST;
                    break;
                }
                else
                {
                    chat.manageReaction(msg, reaction, OP_DELREACTION);
                }
            }

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_IMPORT_MESSAGES:
        {
            if (mClient->initState() != karere::Client::kInitHasOfflineSession
                            && mClient->initState() != karere::Client::kInitHasOnlineSession)
            {
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            int count = mClient->importMessages(request->getText());
            if (count < 0)
            {
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            request->setNumber(count);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
#ifndef KARERE_DISABLE_WEBRTC
        case MegaChatRequest::TYPE_ENABLE_AUDIO_LEVEL_MONITOR:
        {
            handle chatid = request->getChatHandle();
            bool enable = request->getFlag();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_ENABLE_AUDIO_LEVEL_MONITOR - Invalid chatid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("Enable audio level monitor  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (!call->participate())
            {
                API_LOG_ERROR("Enable audio level monitor - You don't participate in the call");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            call->enableAudioLevelMonitor(enable);
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_REQUEST_SPEAK:
        {
            handle chatid = request->getChatHandle();
            bool enable = request->getFlag();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_SPEAK - Invalid chatid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_SPEAK  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (call->getState() != rtcModule::kStateInProgress)
            {
                API_LOG_ERROR("Request to speak - Call isn't in progress state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            call->requestSpeaker(enable);
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_APPROVE_SPEAK:
        {
            handle chatid = request->getChatHandle();

            bool enable = request->getFlag();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_APPROVE_SPEAK - Invalid chatid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_APPROVE_SPEAK  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_APPROVE_SPEAK- Chatroom has not been found");
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            bool moderator = chatroom->chat().getOwnprivilege() == PRIV_OPER;
            if (!moderator)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_APPROVE_SPEAK  - You have to be moderator to approve speak request");
                errorCode = MegaChatError::ERROR_ACCESS;
                assert(false);
                break;
            }

            if (call->getState() != rtcModule::kStateInProgress)
            {
                API_LOG_ERROR("Approve request to speak - Call isn't in progress state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            call->approveSpeakRequest(request->getUserHandle(), enable);


            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_REQUEST_HIGH_RES_VIDEO:
        {
            handle chatid = request->getChatHandle();
            int quality = request->getPrivilege(); // by default MegaChatCall::CALL_QUALITY_HIGH_DEF

            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIGH_RES_VIDEO - Invalid chatid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (request->getFlag() && (quality < MegaChatCall::CALL_QUALITY_HIGH_DEF || quality > MegaChatCall::CALL_QUALITY_HIGH_LOW))
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIGH_RES_VIDEO  - Invalid resolution quality");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIGH_RES_VIDEO  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (!request->getFlag() && (!request->getMegaHandleList() || !request->getMegaHandleList()->size()))
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIGH_RES_VIDEO - Invalid list of Cids for removal");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            if (call->getState() != rtcModule::kStateInProgress)
            {
                API_LOG_ERROR("Request high resolution video - Call isn't in progress state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (request->getFlag()) // HI-RES request only accepts a single peer CID
            {
                Cid_t cid = static_cast<Cid_t>(request->getUserHandle());
                call->requestHighResolutionVideo(cid, quality);
            }
            else // HI-RES del accepts a list of peers CIDs
            {
                std::vector<Cid_t> cids;
                const MegaHandleList *auxcids = request->getMegaHandleList();
                for (size_t i = 0; i < auxcids->size(); i++)
                {
                    cids.emplace_back(static_cast<Cid_t>(auxcids->get(static_cast<unsigned>(i))));
                }
                call->stopHighResolutionVideo(cids);
            }

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_REQUEST_LOW_RES_VIDEO:
        {
            handle chatid = request->getChatHandle();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_LOW_RES_VIDEO - Invalid chatid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall *call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_LOW_RES_VIDEO  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (call->getState() != rtcModule::kStateInProgress)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_LOW_RES_VIDEO - Call isn't in progress state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (!request->getMegaHandleList() || !request->getMegaHandleList()->size())
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_LOW_RES_VIDEO - Invalid list of Cids");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            std::vector<Cid_t> cids;
            const MegaHandleList *auxcids = request->getMegaHandleList();
            for (size_t i = 0; i < auxcids->size(); i++)
            {
                cids.emplace_back(static_cast<Cid_t>(auxcids->get(static_cast<unsigned>(i))));
            }

            if (request->getFlag())
            {
                call->requestLowResolutionVideo(cids);
            }
            else
            {
                call->stopLowResolutionVideo(cids);
            }

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }

        case MegaChatRequest::TYPE_OPEN_VIDEO_DEVICE:
        {
            if (!mClient->rtc)
            {
                API_LOG_ERROR("OpenVideoDevice - WebRTC is not initialized");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            if (request->getFlag())
            {
                mClient->rtc->takeDevice();
            }
            else
            {
                mClient->rtc->releaseDevice();
            }

            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }

        case MegaChatRequest::TYPE_REQUEST_HIRES_QUALITY:
        {
            handle chatid = request->getChatHandle();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIRES_QUALITY - Invalid chatid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall* call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIRES_QUALITY  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (call->getState() != rtcModule::kStateInProgress)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIRES_QUALITY - Call isn't in progress state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            Cid_t cid = static_cast<Cid_t>(request->getUserHandle());
            if (!call->hasVideoSlot(cid, true))
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIRES_QUALITY  - Currently not receiving a hi-res stream for this peer");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            int quality = request->getParamType();
            if (quality < MegaChatCall::CALL_QUALITY_HIGH_DEF || quality > MegaChatCall::CALL_QUALITY_HIGH_LOW)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_REQUEST_HIRES_QUALITY  - invalid quality level value (spatial layer offset).");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            call->requestHiResQuality(cid, quality);
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
        case MegaChatRequest::TYPE_DEL_SPEAKER:
        {
            handle chatid = request->getChatHandle();
            if (chatid == MEGACHAT_INVALID_HANDLE)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_DEL_SPEAKER - Invalid chatid");
                errorCode = MegaChatError::ERROR_ARGS;
                break;
            }

            rtcModule::ICall *call = findCall(chatid);
            if (!call)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_DEL_SPEAKER  - There is not any call in that chatroom");
                errorCode = MegaChatError::ERROR_NOENT;
                assert(false);
                break;
            }

            if (call->getState() != rtcModule::kStateInProgress)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_DEL_SPEAKER - Call isn't in progress state");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            Cid_t cid = request->getUserHandle() != MEGACHAT_INVALID_HANDLE
                    ? static_cast<Cid_t>(request->getUserHandle())
                    : 0; // own user

            ChatRoom *chatroom = findChatRoom(chatid);
            if (!chatroom)
            {
                errorCode = MegaChatError::ERROR_NOENT;
                break;
            }

            if (chatroom->ownPriv() != MegaChatPeerList::PRIV_MODERATOR
                    && cid)
            {
                API_LOG_ERROR("MegaChatRequest::TYPE_DEL_SPEAKER - You don't have enough permisions to remove the speaker");
                errorCode = MegaChatError::ERROR_ACCESS;
                break;
            }

            call->stopSpeak(cid);
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(MegaChatError::ERROR_OK);
            fireOnChatRequestFinish(request, megaChatError);
            break;
        }
#endif
        default:
        {
            errorCode = MegaChatError::ERROR_UNKNOWN;
        }
        }   // end of switch(request->getType())

        if(errorCode)
        {
            MegaChatErrorPrivate *megaChatError = new MegaChatErrorPrivate(errorCode);
            API_LOG_WARNING("Error starting request: %s", megaChatError->getErrorString());
            fireOnChatRequestFinish(request, megaChatError);
        }
    }
}

void MegaChatApiImpl::sendPendingEvents()
{
    megaMessage* msg;
    while ((msg = eventQueue.pop()))
    {
        megaProcessMessage(msg);
    }
}

void MegaChatApiImpl::setLogLevel(int logLevel)
{
    if (!loggerHandler)
    {
        loggerHandler = new LoggerHandler();
    }
    loggerHandler->setLogLevel(logLevel);
}

void MegaChatApiImpl::setLogWithColors(bool useColors)
{
    if (loggerHandler)
    {
        loggerHandler->setLogWithColors(useColors);
    }
}

void MegaChatApiImpl::setLogToConsole(bool enable)
{
    if (loggerHandler)
    {
        loggerHandler->setLogToConsole(enable);
    }
}

void MegaChatApiImpl::setLoggerClass(MegaChatLogger *megaLogger)
{
    if (!megaLogger)   // removing logger
    {
        delete loggerHandler;
        loggerHandler = NULL;
    }
    else
    {
        if (!loggerHandler)
        {
            loggerHandler = new LoggerHandler();
        }
        loggerHandler->setMegaChatLogger(megaLogger);
    }
}

int MegaChatApiImpl::initAnonymous()
{
    sdkMutex.lock();
    createKarereClient();

    int state = mClient->initWithAnonymousSession();
    if (state != karere::Client::kInitAnonymousMode)
    {
        // there's been an error during initialization
        localLogout();
    }

    sdkMutex.unlock();
    return MegaChatApiImpl::convertInitState(state);
}

int MegaChatApiImpl::init(const char *sid, bool waitForFetchnodesToConnect)
{
    sdkMutex.lock();
    createKarereClient();

    int state = mClient->init(sid, waitForFetchnodesToConnect);
    if (state != karere::Client::kInitErrNoCache &&
            state != karere::Client::kInitWaitingNewSession &&
            state != karere::Client::kInitHasOfflineSession)
    {
        // there's been an error during initialization
        localLogout();
    }

    sdkMutex.unlock();
    return MegaChatApiImpl::convertInitState(state);
}

void MegaChatApiImpl::resetClientid()
{
    sdkMutex.lock();
    if (mClient)
    {
        mClient->resetMyIdentity();
    }
    sdkMutex.unlock();
}

void MegaChatApiImpl::createKarereClient()
{
    if (!mClient)
    {
#ifndef KARERE_DISABLE_WEBRTC
        uint8_t caps = karere::kClientIsMobile | karere::kClientCanWebrtc | karere::kClientSupportLastGreen;
#else
        uint8_t caps = karere::kClientIsMobile | karere::kClientSupportLastGreen;
#endif
        mClient = new karere::Client(*mMegaApi, mWebsocketsIO, *this, *this, mMegaApi->getBasePath(), caps, this);
        mTerminating = false;
    }
}

int MegaChatApiImpl::getInitState()
{
    int initState;

    sdkMutex.lock();
    if (mClient)
    {
        initState = MegaChatApiImpl::convertInitState(mClient->initState());
    }
    else
    {
        initState = MegaChatApi::INIT_NOT_DONE;
    }
    sdkMutex.unlock();

    return initState;
}

void MegaChatApiImpl::importMessages(const char *externalDbPath, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_IMPORT_MESSAGES, listener);
    request->setText(externalDbPath);
    requestQueue.push(request);
    waiter->notify();
}

MegaChatRoomHandler *MegaChatApiImpl::getChatRoomHandler(MegaChatHandle chatid)
{
    map<MegaChatHandle, MegaChatRoomHandler*>::iterator it = chatRoomHandler.find(chatid);
    if (it == chatRoomHandler.end())
    {
        chatRoomHandler[chatid] = new MegaChatRoomHandler(this, mChatApi, mMegaApi, chatid);
    }

    return chatRoomHandler[chatid];
}

void MegaChatApiImpl::removeChatRoomHandler(MegaChatHandle chatid)
{
    map<MegaChatHandle, MegaChatRoomHandler*>::iterator it = chatRoomHandler.find(chatid);
    if (it == chatRoomHandler.end())
    {
        API_LOG_WARNING("removeChatRoomHandler: chatroom handler not found (chatid: %s)", ID_CSTR(chatid));
        return;
    }

    MegaChatRoomHandler *roomHandler = chatRoomHandler[chatid];
    chatRoomHandler.erase(it);
    delete roomHandler;
}

ChatRoom *MegaChatApiImpl::findChatRoom(MegaChatHandle chatid)
{
    ChatRoom *chatroom = NULL;

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it = mClient->chats->find(chatid);
        if (it != mClient->chats->end())
        {
            chatroom = it->second;
        }
    }

    sdkMutex.unlock();

    return chatroom;
}

karere::ChatRoom *MegaChatApiImpl::findChatRoomByUser(MegaChatHandle userhandle)
{
    ChatRoom *chatroom = NULL;

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ContactList::iterator it = mClient->mContactList->find(userhandle);
        if (it != mClient->mContactList->end())
        {
            chatroom = it->second->chatRoom();
        }
    }

    sdkMutex.unlock();

    return chatroom;
}

chatd::Message *MegaChatApiImpl::findMessage(MegaChatHandle chatid, MegaChatHandle msgid)
{
    Message *msg = NULL;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Idx index = chat.msgIndexFromId(msgid);
        if (index != CHATD_IDX_INVALID)
        {
            msg = chat.findOrNull(index);
        }
    }

    sdkMutex.unlock();

    return msg;
}

chatd::Message *MegaChatApiImpl::findMessageNotConfirmed(MegaChatHandle chatid, MegaChatHandle msgxid)
{
    Message *msg = NULL;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        msg = chat.getMsgByXid(msgxid);
    }

    sdkMutex.unlock();

    return msg;
}

void MegaChatApiImpl::setCatchException(bool enable)
{
    karere::gCatchException = enable;
}

bool MegaChatApiImpl::hasUrl(const char *text)
{
    std::string url;
    return chatd::Message::hasUrl(text, url);
}

bool MegaChatApiImpl::openNodeHistory(MegaChatHandle chatid, MegaChatNodeHistoryListener *listener)
{
    if (!listener || chatid == MEGACHAT_INVALID_HANDLE)
    {
        return false;
    }

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        auto it = nodeHistoryHandlers.find(chatid);
        if (it != nodeHistoryHandlers.end())
        {
            sdkMutex.unlock();
            API_LOG_WARNING("openNodeHistory: node history is already open for this chatroom (chatid: %s), close it before open it again", karere::Id(chatid).toString().c_str());
            throw std::runtime_error("App node history handler is already set, remove it first");
            return false;
        }

        MegaChatNodeHistoryHandler *handler = new MegaChatNodeHistoryHandler(mChatApi);
        chatroom->chat().setNodeHistoryHandler(handler);
        nodeHistoryHandlers[chatid] = handler;
        handler->addMegaNodeHistoryListener(listener);
    }

    sdkMutex.unlock();
    return chatroom;
}

bool MegaChatApiImpl::closeNodeHistory(MegaChatHandle chatid, MegaChatNodeHistoryListener *listener)
{
    if (!listener || chatid == MEGACHAT_INVALID_HANDLE)
    {
        return false;
    }

    sdkMutex.lock();
    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        auto it = nodeHistoryHandlers.find(chatid);
        if (it != nodeHistoryHandlers.end())
        {
            MegaChatNodeHistoryHandler *handler = it->second;
            nodeHistoryHandlers.erase(it);
            delete handler;
            chatroom->chat().unsetHandlerToNodeHistory();

            sdkMutex.unlock();
            return true;
        }
    }

    sdkMutex.unlock();
    return false;
}

void MegaChatApiImpl::addNodeHistoryListener(MegaChatHandle chatid, MegaChatNodeHistoryListener *listener)
{
    if (!listener || chatid == MEGACHAT_INVALID_HANDLE)
    {
        return;
    }

    sdkMutex.lock();
    auto it = nodeHistoryHandlers.find(chatid);
    if (it != nodeHistoryHandlers.end())
    {
        MegaChatNodeHistoryHandler *handler = it->second;
        handler->addMegaNodeHistoryListener(listener);

    }
    else
    {
        assert(false);
        API_LOG_WARNING("addNodeHistoryListener: node history handler not found (chatid: %s)", karere::Id(chatid).toString().c_str());
    }

    sdkMutex.unlock();
}

void MegaChatApiImpl::removeNodeHistoryListener(MegaChatHandle chatid, MegaChatNodeHistoryListener *listener)
{
    if (!listener || chatid == MEGACHAT_INVALID_HANDLE)
    {
        return;
    }

    sdkMutex.lock();
    auto it = nodeHistoryHandlers.find(chatid);
    if (it != nodeHistoryHandlers.end())
    {
        MegaChatNodeHistoryHandler *handler = it->second;
        handler->removeMegaNodeHistoryListener(listener);

    }
    else
    {
        assert(false);
        API_LOG_WARNING("removeNodeHistoryListener: node history handler not found (chatid: %s)", karere::Id(chatid).toString().c_str());
    }

    sdkMutex.unlock();

}

int MegaChatApiImpl::loadAttachments(MegaChatHandle chatid, int count)
{
    int ret = MegaChatApi::SOURCE_NONE;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        HistSource source = chat.getNodeHistory(count);
        switch (source)
        {
        case kHistSourceNone:   ret = MegaChatApi::SOURCE_NONE; break;
        case kHistSourceRam:
        case kHistSourceDb:     ret = MegaChatApi::SOURCE_LOCAL; break;
        case kHistSourceServer: ret = MegaChatApi::SOURCE_REMOTE; break;
        case kHistSourceNotLoggedIn: ret = MegaChatApi::SOURCE_ERROR; break;
        default:
            API_LOG_ERROR("Unknown source of messages at loadAttachments()");
            break;
        }
    }

    sdkMutex.unlock();
    return ret;
}

void MegaChatApiImpl::fireOnChatRequestStart(MegaChatRequestPrivate *request)
{
    API_LOG_INFO("Request (%s) starting", request->getRequestString());

    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestStart(mChatApi, request);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestStart(mChatApi, request);
    }
}

void MegaChatApiImpl::fireOnChatRequestFinish(MegaChatRequestPrivate *request, MegaChatError *e)
{
    if(e->getErrorCode())
    {
        API_LOG_INFO("Request (%s) finished with error: %s", request->getRequestString(), e->getErrorString());
    }
    else
    {
        API_LOG_INFO("Request (%s) finished", request->getRequestString());
    }

    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestFinish(mChatApi, request, e);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestFinish(mChatApi, request, e);
    }

    requestMap.erase(request->getTag());

    delete request;
    delete e;
}

void MegaChatApiImpl::fireOnChatRequestUpdate(MegaChatRequestPrivate *request)
{
    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestUpdate(mChatApi, request);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestUpdate(mChatApi, request);
    }
}

void MegaChatApiImpl::fireOnChatRequestTemporaryError(MegaChatRequestPrivate *request, MegaChatError *e)
{
    request->setNumRetry(request->getNumRetry() + 1);

    for (set<MegaChatRequestListener *>::iterator it = requestListeners.begin(); it != requestListeners.end() ; it++)
    {
        (*it)->onRequestTemporaryError(mChatApi, request, e);
    }

    MegaChatRequestListener* listener = request->getListener();
    if (listener)
    {
        listener->onRequestTemporaryError(mChatApi, request, e);
    }

    delete e;
}

#ifndef KARERE_DISABLE_WEBRTC

void MegaChatApiImpl::fireOnChatCallUpdate(MegaChatCallPrivate *call)
{
    if (call->getCallId() == Id::inval())
    {
        // if a call have no id yet, it's because we haven't received yet the initial CALLDATA,
        // but just some previous opcodes related to the call, like INCALLs or CALLTIME (which
        // do not include the callid)
        return;
    }

    if (mTerminating)
    {
        return;
    }

    for (set<MegaChatCallListener *>::iterator it = callListeners.begin(); it != callListeners.end() ; it++)
    {
        (*it)->onChatCallUpdate(mChatApi, call);
    }

    if (call->hasChanged(MegaChatCall::CHANGE_TYPE_STATUS)
            && (call->isRinging()              // for callee, incoming call
                || call->getStatus() == MegaChatCall::CALL_STATUS_USER_NO_PRESENT   // for callee (groupcalls)
                || call->getStatus() == MegaChatCall::CALL_STATUS_DESTROYED))       // call finished
    {
        // notify at MegaChatListItem level about new calls and calls being terminated
        ChatRoom *room = findChatRoom(call->getChatid());
        MegaChatListItemPrivate *item = new MegaChatListItemPrivate(*room);
        item->setCallInProgress();

        fireOnChatListItemUpdate(item);
    }

    call->removeChanges();
}

void MegaChatApiImpl::fireOnChatSessionUpdate(MegaChatHandle chatid, MegaChatHandle callid, MegaChatSessionPrivate *session)
{
    if (mTerminating)
    {
        return;
    }

    for (set<MegaChatCallListener *>::iterator it = callListeners.begin(); it != callListeners.end() ; it++)
    {
        (*it)->onChatSessionUpdate(mChatApi, chatid, callid, session);
    }

    session->removeChanges();
}

void MegaChatApiImpl::fireOnChatVideoData(MegaChatHandle chatid, uint32_t clientId, int width, int height, char *buffer, rtcModule::VideoResolution videoResolution)
{
    std::map<MegaChatHandle, MegaChatPeerVideoListener_map>::iterator it;
    std::map<MegaChatHandle, MegaChatPeerVideoListener_map>::iterator itEnd;
    assert(videoResolution != rtcModule::VideoResolution::kUndefined);
    if (clientId == 0)
    {
        for( MegaChatVideoListener_set::iterator videoListenerIterator = mLocalVideoListeners[chatid].begin();
             videoListenerIterator != mLocalVideoListeners[chatid].end();
             videoListenerIterator++)
        {
            if (*videoListenerIterator == nullptr)
            {
                API_LOG_WARNING("local videoListener does not exists");
                continue;
            }
            (*videoListenerIterator)->onChatVideoData(mChatApi, chatid, width, height, buffer, width * height * 4);
        }

        return;
    }

    if (videoResolution == rtcModule::VideoResolution::kHiRes)
    {
         it = mVideoListenersHiRes.find(chatid);
         itEnd = mVideoListenersHiRes.end();
    }
    else
    {
        assert(clientId); // local video listeners can't be un/registered into this map
        it = mVideoListenersLowRes.find(chatid);
        itEnd = mVideoListenersLowRes.end();
    }

    if (it != itEnd)
    {
        MegaChatPeerVideoListener_map::iterator peerVideoIterator = it->second.find(clientId);
        if (peerVideoIterator != it->second.end())
        {
            for( MegaChatVideoListener_set::iterator videoListenerIterator = peerVideoIterator->second.begin();
                 videoListenerIterator != peerVideoIterator->second.end();
                 videoListenerIterator++)
            {
                if (*videoListenerIterator == nullptr)
                {
                    API_LOG_WARNING("remote videoListener with CID %d does not exists ", clientId);
                    continue;
                }

                (*videoListenerIterator)->onChatVideoData(mChatApi, chatid, width, height, buffer, width * height * 4);
            }
        }
    }
}

#endif  // webrtc

void MegaChatApiImpl::fireOnChatListItemUpdate(MegaChatListItem *item)
{
    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatListItemUpdate(mChatApi, item);
    }

    delete item;
}

void MegaChatApiImpl::fireOnChatInitStateUpdate(int newState)
{
    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatInitStateUpdate(mChatApi, newState);
    }
}

void MegaChatApiImpl::fireOnChatOnlineStatusUpdate(MegaChatHandle userhandle, int status, bool inProgress)
{
    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatOnlineStatusUpdate(mChatApi, userhandle, status, inProgress);
    }
}

void MegaChatApiImpl::fireOnChatPresenceConfigUpdate(MegaChatPresenceConfig *config)
{
    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatPresenceConfigUpdate(mChatApi, config);
    }

    delete config;
}

void MegaChatApiImpl::fireOnChatPresenceLastGreenUpdated(MegaChatHandle userhandle, int lastGreen)
{
    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatPresenceLastGreen(mChatApi, userhandle, lastGreen);
    }
}

void MegaChatApiImpl::fireOnChatConnectionStateUpdate(MegaChatHandle chatid, int newState)
{
    bool allConnected = (newState == MegaChatApi::CHAT_CONNECTION_ONLINE) ? mClient->mChatdClient->areAllChatsLoggedIn() : false;

    for(set<MegaChatListener *>::iterator it = listeners.begin(); it != listeners.end() ; it++)
    {
        (*it)->onChatConnectionStateUpdate(mChatApi, chatid, newState);

        if (allConnected)
        {
            (*it)->onChatConnectionStateUpdate(mChatApi, MEGACHAT_INVALID_HANDLE, newState);
        }
    }
}

void MegaChatApiImpl::fireOnChatNotification(MegaChatHandle chatid, MegaChatMessage *msg)
{
    for(set<MegaChatNotificationListener *>::iterator it = notificationListeners.begin(); it != notificationListeners.end() ; it++)
    {
        (*it)->onChatNotification(mChatApi, chatid, msg);
    }

    delete msg;
}

void MegaChatApiImpl::connect(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CONNECT, listener);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::connectInBackground(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CONNECT, listener);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::disconnect(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_DISCONNECT, listener);
    requestQueue.push(request);
    waiter->notify();
}

int MegaChatApiImpl::getConnectionState()
{
    int ret = 0;

    sdkMutex.lock();
    ret = mClient ? (int) mClient->connState() : MegaChatApi::DISCONNECTED;
    sdkMutex.unlock();

    return ret;
}

int MegaChatApiImpl::getChatConnectionState(MegaChatHandle chatid)
{
    int ret = MegaChatApi::CHAT_CONNECTION_OFFLINE;

    sdkMutex.lock();
    ChatRoom *room = findChatRoom(chatid);
    if (room)
    {
        ret = MegaChatApiImpl::convertChatConnectionState(room->chatdOnlineState());
    }
    sdkMutex.unlock();

    return ret;
}

bool MegaChatApiImpl::areAllChatsLoggedIn()
{
    sdkMutex.lock();
    bool ret = mClient->mChatdClient->areAllChatsLoggedIn();
    sdkMutex.unlock();

    return ret;
}

void MegaChatApiImpl::retryPendingConnections(bool disconnect, bool refreshURL, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_RETRY_PENDING_CONNECTIONS, listener);
    request->setFlag(disconnect);
    request->setParamType(refreshURL ? 1 : 0);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::logout(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_LOGOUT, listener);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::localLogout(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_LOGOUT, listener);
    request->setFlag(false);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setOnlineStatus(int status, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_ONLINE_STATUS, listener);
    request->setNumber(status);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setPresenceAutoaway(bool enable, int64_t timeout, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_PRESENCE_AUTOAWAY, listener);
    request->setFlag(enable);
    request->setNumber(timeout);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setPresencePersist(bool enable, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_PRESENCE_PERSIST, listener);
    request->setFlag(enable);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::signalPresenceActivity(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SIGNAL_ACTIVITY, listener);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setLastGreenVisible(bool enable, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_LAST_GREEN_VISIBLE, listener);
    request->setFlag(enable);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::requestLastGreen(MegaChatHandle userid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_LAST_GREEN, listener);
    request->setUserHandle(userid);
    requestQueue.push(request);
    waiter->notify();
}

MegaChatPresenceConfig *MegaChatApiImpl::getPresenceConfig()
{
    MegaChatPresenceConfigPrivate *config = NULL;

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        const ::presenced::Config &cfg = mClient->presenced().config();
        if (cfg.presence().isValid())
        {
            config = new MegaChatPresenceConfigPrivate(cfg, mClient->presenced().isConfigAcknowledged());
        }
    }

    sdkMutex.unlock();

    return config;
}

bool MegaChatApiImpl::isSignalActivityRequired()
{
    sdkMutex.lock();

    bool enabled = mClient ? mClient->presenced().isSignalActivityRequired() : false;

    sdkMutex.unlock();

    return enabled;
}

int MegaChatApiImpl::getOnlineStatus()
{
    sdkMutex.lock();

    int status = mClient ? getUserOnlineStatus(mClient->myHandle()) : (int)MegaChatApi::STATUS_INVALID;

    sdkMutex.unlock();

    return status;
}

bool MegaChatApiImpl::isOnlineStatusPending()
{
    sdkMutex.lock();

    bool statusInProgress = mClient ? mClient->presenced().isConfigAcknowledged() : false;

    sdkMutex.unlock();

    return statusInProgress;
}

int MegaChatApiImpl::getUserOnlineStatus(MegaChatHandle userhandle)
{
    int status = MegaChatApi::STATUS_INVALID;

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        status = mClient->presenced().peerPresence(userhandle).status();
    }

    sdkMutex.unlock();

    return status;
}

void MegaChatApiImpl::setBackgroundStatus(bool background, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_BACKGROUND_STATUS, listener);
    request->setFlag(background);
    requestQueue.push(request);
    waiter->notify();
}

int MegaChatApiImpl::getBackgroundStatus()
{
    sdkMutex.lock();

    int status = mClient ? int(mClient->isInBackground()) : -1;

    sdkMutex.unlock();

    return status;
}

void MegaChatApiImpl::getUserFirstname(MegaChatHandle userhandle, const char *authorizationToken, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_GET_FIRSTNAME, listener);
    request->setUserHandle(userhandle);
    request->setLink(authorizationToken);
    requestQueue.push(request);
    waiter->notify();
}

const char *MegaChatApiImpl::getUserFirstnameFromCache(MegaChatHandle userhandle)
{
    SdkMutexGuard g(sdkMutex);
    if (mClient && mClient->isUserAttrCacheReady())
    {
        const Buffer* buffer = mClient->userAttrCache().getDataFromCache(userhandle, ::mega::MegaApi::USER_ATTR_FIRSTNAME);
        return buffer ? buffer->c_str() : nullptr;

    }

    return nullptr;
}

void MegaChatApiImpl::getUserLastname(MegaChatHandle userhandle, const char *authorizationToken, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_GET_LASTNAME, listener);
    request->setUserHandle(userhandle);
    request->setLink(authorizationToken);
    requestQueue.push(request);
    waiter->notify();
}

const char *MegaChatApiImpl::getUserLastnameFromCache(MegaChatHandle userhandle)
{
    SdkMutexGuard g(sdkMutex);
    if (mClient && mClient->isUserAttrCacheReady())
    {
        const Buffer* buffer = mClient->userAttrCache().getDataFromCache(userhandle, ::mega::MegaApi::USER_ATTR_LASTNAME);
        return buffer ? buffer->c_str() : nullptr;
    }

    return nullptr;
}

const char *MegaChatApiImpl::getUserFullnameFromCache(MegaChatHandle userhandle)
{
    SdkMutexGuard g(sdkMutex);
    if (mClient && mClient->isUserAttrCacheReady())
    {
        const Buffer* buffer = mClient->userAttrCache().getDataFromCache(userhandle, USER_ATTR_FULLNAME);
        if (buffer != nullptr)
        {
            std::string fullname(buffer->buf()+1, buffer->dataSize()-1);
            return MegaApi::strdup(fullname.c_str());
        }
    }

    return nullptr;
}

void MegaChatApiImpl::getUserEmail(MegaChatHandle userhandle, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_GET_EMAIL, listener);
    request->setUserHandle(userhandle);
    requestQueue.push(request);
    waiter->notify();
}

const char *MegaChatApiImpl::getUserEmailFromCache(MegaChatHandle userhandle)
{
    SdkMutexGuard g(sdkMutex);
    if (mClient && mClient->isUserAttrCacheReady())
    {
        const Buffer* buffer = mClient->userAttrCache().getDataFromCache(userhandle, USER_ATTR_EMAIL);
        return buffer ? buffer->c_str() : nullptr;
    }

    return nullptr;
}

const char *MegaChatApiImpl::getUserAliasFromCache(MegaChatHandle userhandle)
{
    SdkMutexGuard g(sdkMutex);
    if (mClient && mClient->isUserAttrCacheReady())
    {
        const Buffer* buffer = mClient->userAttrCache().getDataFromCache(mClient->myHandle(), ::mega::MegaApi::USER_ATTR_ALIAS);

        if (!buffer || buffer->empty()) return nullptr;

        const std::string container(buffer->buf(), buffer->size());
        std::unique_ptr<::mega::TLVstore> tlvRecords(::mega::TLVstore::containerToTLVrecords(&container));
        std::unique_ptr<std::vector<std::string>> keys(tlvRecords->getKeys());

        for (auto &key : *keys)
        {
            Id userid(key.data());
            if (userid == userhandle)
            {
                string value;
                tlvRecords->get(key.c_str(), value);
                return MegaApi::strdup(value.c_str());
            }
        }
    }

    return nullptr;
}

MegaStringMap *MegaChatApiImpl::getUserAliasesFromCache()
{
    SdkMutexGuard g(sdkMutex);
    if (mClient && mClient->isUserAttrCacheReady())
    {
        const Buffer* buffer = mClient->userAttrCache().getDataFromCache(mClient->myHandle(), ::mega::MegaApi::USER_ATTR_ALIAS);

        if (!buffer || buffer->empty()) return nullptr;

        const std::string container(buffer->buf(), buffer->size());
        std::unique_ptr<::mega::TLVstore> tlvRecords(::mega::TLVstore::containerToTLVrecords(&container));
        return new MegaStringMapPrivate(tlvRecords->getMap(), true);
    }

    return nullptr;
}

void MegaChatApiImpl::loadUserAttributes(MegaChatHandle chatid, MegaHandleList *userList, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_GET_PEER_ATTRIBUTES, listener);
    request->setChatHandle(chatid);
    request->setMegaHandleList(userList);
    requestQueue.push(request);
    waiter->notify();
}

unsigned int MegaChatApiImpl::getMaxParticipantsWithAttributes()
{
    return PRELOAD_CHATLINK_PARTICIPANTS;
}

char *MegaChatApiImpl::getContactEmail(MegaChatHandle userhandle)
{
    char *ret = NULL;

    sdkMutex.lock();

    const std::string *email = mClient ? mClient->mContactList->getUserEmail(userhandle) : NULL;
    if (email)
    {
        ret = MegaApi::strdup(email->c_str());
    }

    sdkMutex.unlock();

    return ret;
}

MegaChatHandle MegaChatApiImpl::getUserHandleByEmail(const char *email)
{
    MegaChatHandle uh = MEGACHAT_INVALID_HANDLE;

    if (email)
    {
        sdkMutex.lock();

        Contact *contact = mClient ? mClient->mContactList->contactFromEmail(email) : NULL;
        if (contact)
        {
            uh = contact->userId();
        }

        sdkMutex.unlock();
    }

    return uh;
}

MegaChatHandle MegaChatApiImpl::getMyUserHandle()
{
    return mClient ? (MegaChatHandle) mClient->myHandle() : MEGACHAT_INVALID_HANDLE;
}

MegaChatHandle MegaChatApiImpl::getMyClientidHandle(MegaChatHandle chatid)
{
    MegaChatHandle clientid = 0;
    sdkMutex.lock();
    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        clientid = chatroom->chat().connection().clientId();
    }

    sdkMutex.unlock();

    return clientid;
}

char *MegaChatApiImpl::getMyFirstname()
{
    if (!mClient)
    {
        return NULL;
    }

    return MegaChatRoomPrivate::firstnameFromBuffer(mClient->myName());
}

char *MegaChatApiImpl::getMyLastname()
{
    if (!mClient)
    {
        return NULL;
    }

    return MegaChatRoomPrivate::lastnameFromBuffer(mClient->myName());
}

char *MegaChatApiImpl::getMyFullname()
{
    if (!mClient)
    {
        return NULL;
    }

    return MegaApi::strdup(mClient->myName().substr(1).c_str());
}

char *MegaChatApiImpl::getMyEmail()
{
    if (!mClient)
    {
        return NULL;
    }

    return MegaApi::strdup(mClient->myEmail().c_str());
}

MegaChatRoomList *MegaChatApiImpl::getChatRooms()
{
    MegaChatRoomListPrivate *chats = new MegaChatRoomListPrivate();

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            chats->addChatRoom(new MegaChatRoomPrivate(*it->second));
        }
    }

    sdkMutex.unlock();

    return chats;
}

MegaChatRoom *MegaChatApiImpl::getChatRoom(MegaChatHandle chatid)
{
    MegaChatRoomPrivate *chat = NULL;

    sdkMutex.lock();

    ChatRoom *chatRoom = findChatRoom(chatid);
    if (chatRoom)
    {
        chat = new MegaChatRoomPrivate(*chatRoom);
    }

    sdkMutex.unlock();

    return chat;
}

MegaChatRoom *MegaChatApiImpl::getChatRoomByUser(MegaChatHandle userhandle)
{
    MegaChatRoomPrivate *chat = NULL;

    sdkMutex.lock();

    ChatRoom *chatRoom = findChatRoomByUser(userhandle);
    if (chatRoom)
    {
        chat = new MegaChatRoomPrivate(*chatRoom);
    }

    sdkMutex.unlock();

    return chat;
}

MegaChatListItemList *MegaChatApiImpl::getChatListItems()
{
    MegaChatListItemListPrivate *items = new MegaChatListItemListPrivate();

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            if (!it->second->isArchived())
            {
                items->addChatListItem(new MegaChatListItemPrivate(*it->second));
            }
        }
    }

    sdkMutex.unlock();

    return items;
}

MegaChatListItemList *MegaChatApiImpl::getChatListItemsByPeers(MegaChatPeerList *peers)
{
    MegaChatListItemListPrivate *items = new MegaChatListItemListPrivate();

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            bool sameParticipants = true;
            if (it->second->isGroup())
            {
                GroupChatRoom *chatroom = (GroupChatRoom*) it->second;
                if ((int)chatroom->peers().size() != peers->size())
                {
                    continue;
                }

                for (int i = 0; i < peers->size(); i++)
                {
                    // if the peer in the list is part of the members in the chatroom...
                    MegaChatHandle uh = peers->getPeerHandle(i);
                    if (chatroom->peers().find(uh) == chatroom->peers().end())
                    {
                        sameParticipants = false;
                        break;
                    }
                }
                if (sameParticipants == true)
                {
                    items->addChatListItem(new MegaChatListItemPrivate(*it->second));
                }

            }
            else    // 1on1
            {
                if (peers->size() != 1)
                {
                    continue;
                }

                PeerChatRoom *chatroom = (PeerChatRoom*) it->second;
                if (chatroom->peer() == peers->getPeerHandle(0))
                {
                    items->addChatListItem(new MegaChatListItemPrivate(*it->second));
                }
            }
        }
    }

    sdkMutex.unlock();

    return items;
}

MegaChatListItem *MegaChatApiImpl::getChatListItem(MegaChatHandle chatid)
{
    MegaChatListItemPrivate *item = NULL;

    sdkMutex.lock();

    ChatRoom *chatRoom = findChatRoom(chatid);
    if (chatRoom)
    {
        item = new MegaChatListItemPrivate(*chatRoom);
    }

    sdkMutex.unlock();

    return item;
}

int MegaChatApiImpl::getUnreadChats()
{
    int count = 0;

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            ChatRoom *room = it->second;
            if (!room->isArchived() && !room->previewMode() && room->chat().unreadMsgCount())
            {
                count++;
            }
        }
    }

    sdkMutex.unlock();

    return count;
}

MegaChatListItemList *MegaChatApiImpl::getActiveChatListItems()
{
    MegaChatListItemListPrivate *items = new MegaChatListItemListPrivate();

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            if (!it->second->isArchived() && it->second->isActive())
            {
                items->addChatListItem(new MegaChatListItemPrivate(*it->second));
            }
        }
    }

    sdkMutex.unlock();

    return items;
}

MegaChatListItemList *MegaChatApiImpl::getInactiveChatListItems()
{
    MegaChatListItemListPrivate *items = new MegaChatListItemListPrivate();

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            if (!it->second->isArchived() && !it->second->isActive())
            {
                items->addChatListItem(new MegaChatListItemPrivate(*it->second));
            }
        }
    }

    sdkMutex.unlock();

    return items;
}

MegaChatListItemList *MegaChatApiImpl::getArchivedChatListItems()
{
    MegaChatListItemListPrivate *items = new MegaChatListItemListPrivate();

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            if (it->second->isArchived())
            {
                items->addChatListItem(new MegaChatListItemPrivate(*it->second));
            }
        }
    }

    sdkMutex.unlock();

    return items;
}

MegaChatListItemList *MegaChatApiImpl::getUnreadChatListItems()
{
    MegaChatListItemListPrivate *items = new MegaChatListItemListPrivate();

    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        ChatRoomList::iterator it;
        for (it = mClient->chats->begin(); it != mClient->chats->end(); it++)
        {
            ChatRoom *room = it->second;
            if (!room->isArchived() && room->chat().unreadMsgCount())
            {
                items->addChatListItem(new MegaChatListItemPrivate(*it->second));
            }
        }
    }

    sdkMutex.unlock();

    return items;
}

MegaChatHandle MegaChatApiImpl::getChatHandleByUser(MegaChatHandle userhandle)
{
    MegaChatHandle chatid = MEGACHAT_INVALID_HANDLE;

    sdkMutex.lock();

    ChatRoom *chatRoom = findChatRoomByUser(userhandle);
    if (chatRoom)
    {
        chatid = chatRoom->chatid();
    }

    sdkMutex.unlock();

    return chatid;
}

void MegaChatApiImpl::createChat(bool group, MegaChatPeerList *peerList, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CREATE_CHATROOM, listener);
    request->setFlag(group);
    request->setPrivilege(0);
    request->setMegaChatPeerList(peerList);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::createChat(bool group, MegaChatPeerList *peerList, const char *title, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CREATE_CHATROOM, listener);
    request->setFlag(group);
    request->setPrivilege(0);
    request->setMegaChatPeerList(peerList);
    request->setText(title);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::createPublicChat(MegaChatPeerList *peerList, bool meeting, const char *title, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CREATE_CHATROOM, listener);
    request->setFlag(true);
    request->setPrivilege(1);
    request->setMegaChatPeerList(peerList);
    request->setText(title);
    request->setNumber(meeting);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::chatLinkHandle(MegaChatHandle chatid, bool del, bool createifmissing, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CHAT_LINK_HANDLE, listener);
    request->setChatHandle(chatid);
    request->setFlag(del);
    request->setNumRetry(createifmissing ? 1 : 0);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::inviteToChat(MegaChatHandle chatid, MegaChatHandle uh, int privilege, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_INVITE_TO_CHATROOM, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(uh);
    request->setPrivilege(privilege);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::autojoinPublicChat(MegaChatHandle chatid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_AUTOJOIN_PUBLIC_CHAT, listener);
    request->setChatHandle(chatid);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::autorejoinPublicChat(MegaChatHandle chatid, MegaChatHandle ph, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_AUTOJOIN_PUBLIC_CHAT, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(ph);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::removeFromChat(MegaChatHandle chatid, MegaChatHandle uh, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REMOVE_FROM_CHATROOM, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(uh);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::updateChatPermissions(MegaChatHandle chatid, MegaChatHandle uh, int privilege, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_UPDATE_PEER_PERMISSIONS, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(uh);
    request->setPrivilege(privilege);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::truncateChat(MegaChatHandle chatid, MegaChatHandle messageid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_TRUNCATE_HISTORY, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(messageid);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setChatTitle(MegaChatHandle chatid, const char *title, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_EDIT_CHATROOM_NAME, listener);
    request->setChatHandle(chatid);
    request->setText(title);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::openChatPreview(const char *link, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_LOAD_PREVIEW, listener);
    request->setLink(link);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::checkChatLink(const char *link, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_LOAD_PREVIEW, listener);
    request->setLink(link);
    request->setFlag(false);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setPublicChatToPrivate(MegaChatHandle chatid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_PRIVATE_MODE, listener);
    request->setChatHandle(chatid);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::archiveChat(MegaChatHandle chatid, bool archive, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_ARCHIVE_CHATROOM, listener);
    request->setChatHandle(chatid);
    request->setFlag(archive);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setChatRetentionTime(MegaChatHandle chatid, unsigned period, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_RETENTION_TIME, listener);
    request->setChatHandle(chatid);
    request->setNumber(period);
    requestQueue.push(request);
    waiter->notify();
}

bool MegaChatApiImpl::openChatRoom(MegaChatHandle chatid, MegaChatRoomListener *listener)
{
    if (!listener)
    {
        return false;
    }

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        addChatRoomListener(chatid, listener);
        chatroom->setAppChatHandler(getChatRoomHandler(chatid));
    }

    sdkMutex.unlock();
    return chatroom;
}

void MegaChatApiImpl::closeChatRoom(MegaChatHandle chatid, MegaChatRoomListener *listener)
{
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        chatroom->removeAppChatHandler();
    }
    else
    {
        API_LOG_WARNING("closeChatRoom(): chatid not found [%s]", karere::Id(chatid).toString().c_str());
    }

    removeChatRoomListener(chatid, listener);
    removeChatRoomHandler(chatid);

    sdkMutex.unlock();
}

void MegaChatApiImpl::closeChatPreview(MegaChatHandle chatid)
{
    if (!mClient)
        return;

    SdkMutexGuard g(sdkMutex);

   mClient->chats->removeRoomPreview(chatid);
}

int MegaChatApiImpl::loadMessages(MegaChatHandle chatid, int count)
{
    int ret = MegaChatApi::SOURCE_NONE;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        HistSource source = chat.getHistory(count);
        switch (source)
        {
        case kHistSourceNone:   ret = MegaChatApi::SOURCE_NONE; break;
        case kHistSourceRam:
        case kHistSourceDb:     ret = MegaChatApi::SOURCE_LOCAL; break;
        case kHistSourceServer: ret = MegaChatApi::SOURCE_REMOTE; break;
        case kHistSourceNotLoggedIn: ret = MegaChatApi::SOURCE_ERROR; break;
        default:
            API_LOG_ERROR("Unknown source of messages at loadMessages()");
            break;
        }
    }

    sdkMutex.unlock();
    return ret;
}

bool MegaChatApiImpl::isFullHistoryLoaded(MegaChatHandle chatid)
{
    bool ret = false;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        ret = chat.haveAllHistoryNotified();
    }

    sdkMutex.unlock();
    return ret;
}

MegaChatMessage *MegaChatApiImpl::getMessage(MegaChatHandle chatid, MegaChatHandle msgid)
{
    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Idx index = chat.msgIndexFromId(msgid);
        if (index != CHATD_IDX_INVALID)     // only confirmed messages have index
        {
            Message *msg = chat.findOrNull(index);
            if (msg)
            {
                megaMsg = new MegaChatMessagePrivate(*msg, chat.getMsgStatus(*msg, index), index);
            }
            else
            {
                API_LOG_ERROR("Failed to find message by index, being index retrieved from message id (index: %d, id: %s)", index, ID_CSTR(msgid));
            }
        }
        else    // message still not confirmed, search in sending-queue
        {
            Message *msg = chat.getMsgByXid(msgid);
            if (msg)
            {
                megaMsg = new MegaChatMessagePrivate(*msg, Message::Status::kSending, MEGACHAT_INVALID_INDEX);
            }
            else
            {
                API_LOG_ERROR("Failed to find message by temporal id (id: %s)", ID_CSTR(msgid));
            }
        }
    }
    else
    {
        API_LOG_ERROR("Chatroom not found (chatid: %s)", ID_CSTR(chatid));
    }

    sdkMutex.unlock();
    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::getMessageFromNodeHistory(MegaChatHandle chatid, MegaChatHandle msgid)
{
    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Message *msg = chat.getMessageFromNodeHistory(msgid);
        if (msg)
        {
            Idx idx = chat.getIdxFromNodeHistory(msgid);
            assert(idx != CHATD_IDX_INVALID);
            Message::Status status = (msg->userid == mClient->myHandle()) ? Message::Status::kServerReceived : Message::Status::kSeen;
            megaMsg = new MegaChatMessagePrivate(*msg, status, idx);
        }
        else
        {
            API_LOG_ERROR("Failed to find message at node history (id: %s)", ID_CSTR(msgid));
        }
    }
    else
    {
        API_LOG_ERROR("Chatroom not found (chatid: %s)", ID_CSTR(chatid));
    }

    sdkMutex.unlock();
    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::getManualSendingMessage(MegaChatHandle chatid, MegaChatHandle rowid)
{

    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        chatd::ManualSendReason reason;
        chatd::Message *msg = chat.getManualSending(rowid, reason);
        if (msg)
        {
            megaMsg = new MegaChatMessagePrivate(*msg, chatd::Message::kSendingManual, MEGACHAT_INVALID_INDEX);
            delete msg;

            megaMsg->setStatus(MegaChatMessage::STATUS_SENDING_MANUAL);
            megaMsg->setRowId(rowid);
            megaMsg->setCode(reason);
        }
        else
        {
            API_LOG_ERROR("Message not found (rowid: %d)", rowid);
        }
    }
    else
    {
        API_LOG_ERROR("Chatroom not found (chatid: %s)", ID_CSTR(chatid));
    }

    sdkMutex.unlock();
    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::sendMessage(MegaChatHandle chatid, const char *msg, size_t msgLen, int type)
{
    if (!msg)
    {
        return NULL;
    }

    if (type == Message::kMsgNormal)
    {
        // remove ending carrier-returns
        while (msgLen)
        {
            if (msg[msgLen-1] == '\n' || msg[msgLen-1] == '\r')
            {
                msgLen--;
            }
            else
            {
                break;
            }
        }
    }

    if (!msgLen)
    {
        return NULL;
    }

    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Message *m = chatroom->chat().msgSubmit(msg, msgLen, type, NULL);

        if (!m)
        {
            sdkMutex.unlock();
            return NULL;
        }
        megaMsg = new MegaChatMessagePrivate(*m, Message::Status::kSending, CHATD_IDX_INVALID);
    }

    sdkMutex.unlock();
    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::attachContacts(MegaChatHandle chatid, MegaHandleList *contacts)
{
    if (!mClient)
    {
        return NULL;
    }

    sdkMutex.lock();

    string buf = JSonUtils::generateAttachContactJSon(contacts, mClient->mContactList.get());
    MegaChatMessage *megaMsg = sendMessage(chatid, buf.c_str(), buf.size(), Message::kMsgContact);

    sdkMutex.unlock();

    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::forwardContact(MegaChatHandle sourceChatid, MegaChatHandle msgid, MegaChatHandle targetChatId)
{
    if (!mClient || sourceChatid == MEGACHAT_INVALID_HANDLE || msgid == MEGACHAT_INVALID_HANDLE || targetChatId == MEGACHAT_INVALID_HANDLE)
    {
        return NULL;
    }

    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroomTarget = findChatRoom(targetChatId);
    ChatRoom *chatroomSource = findChatRoom(sourceChatid);
    if (chatroomSource && chatroomTarget)
    {
        chatd::Chat &chat = chatroomSource->chat();
        Idx idx =  chat.msgIndexFromId(msgid);
        chatd::Message *msg = chatroomSource->chat().findOrNull(idx);
        if (msg && msg->type == chatd::Message::kMsgContact)
        {
            std::string contactMsg;
            unsigned char zero = 0x0;
            unsigned char contactType = Message::kMsgContact - Message::kMsgOffset;
            contactMsg.push_back(zero);
            contactMsg.push_back(contactType);
            contactMsg.append(msg->toText());
            Message *m = chatroomTarget->chat().msgSubmit(contactMsg.c_str(), contactMsg.length(), Message::kMsgContact, NULL);
            if (!m)
            {
                sdkMutex.unlock();
                return NULL;
            }
            megaMsg = new MegaChatMessagePrivate(*m, Message::Status::kSending, CHATD_IDX_INVALID);
        }
    }

    sdkMutex.unlock();
    return megaMsg;
}

void MegaChatApiImpl::attachNodes(MegaChatHandle chatid, MegaNodeList *nodes, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_ATTACH_NODE_MESSAGE, listener);
    request->setChatHandle(chatid);
    request->setMegaNodeList(nodes);
    request->setParamType(0);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::attachNode(MegaChatHandle chatid, MegaChatHandle nodehandle, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_ATTACH_NODE_MESSAGE, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(nodehandle);
    request->setParamType(0);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::attachVoiceMessage(MegaChatHandle chatid, MegaChatHandle nodehandle, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_ATTACH_NODE_MESSAGE, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(nodehandle);
    request->setParamType(1);
    requestQueue.push(request);
    waiter->notify();
}

MegaChatMessage * MegaChatApiImpl::sendGeolocation(MegaChatHandle chatid, float longitude, float latitude, const char *img)
{
    string buf = JSonUtils::generateGeolocationJSon(longitude, latitude, img);
    MegaChatMessage *megaMsg = sendMessage(chatid, buf.c_str(), buf.size(), Message::kMsgContainsMeta);
    return megaMsg;
}

MegaChatMessage* MegaChatApiImpl::sendGiphy(MegaChatHandle chatid, const char* srcMp4, const char* srcWebp, long long sizeMp4, long long sizeWebp, int width, int height, const char* title)
{
    if (!srcMp4 || !srcWebp)
    {
        return nullptr;
    }

    if (!sizeMp4 || !sizeWebp)
    {
        return nullptr;
    }

    string buf = JSonUtils::generateGiphyJSon(srcMp4, srcWebp, sizeMp4, sizeWebp, width, height, title);
    MegaChatMessage* megaMsg = sendMessage(chatid, buf.c_str(), buf.size(), Message::kMsgContainsMeta);
    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::editGeolocation(MegaChatHandle chatid, MegaChatHandle msgid, float longitude, float latitude, const char *img)
{
    string buf = JSonUtils::generateGeolocationJSon(longitude, latitude, img);
    MegaChatMessage *megaMsg = editMessage(chatid, msgid, buf.c_str(), buf.size());
    return megaMsg;
}

void MegaChatApiImpl::revokeAttachment(MegaChatHandle chatid, MegaChatHandle handle, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REVOKE_NODE_MESSAGE, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(handle);
    requestQueue.push(request);
    waiter->notify();
}

bool MegaChatApiImpl::isRevoked(MegaChatHandle chatid, MegaChatHandle nodeHandle)
{
    bool ret = false;

    sdkMutex.lock();

    auto it = chatRoomHandler.find(chatid);
    if (it != chatRoomHandler.end())
    {
        ret = it->second->isRevoked(nodeHandle);
    }

    sdkMutex.unlock();

    return ret;
}

MegaChatMessage *MegaChatApiImpl::editMessage(MegaChatHandle chatid, MegaChatHandle msgid, const char *msg, size_t msgLen)
{
    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Message *originalMsg = findMessage(chatid, msgid);
        Idx index;
        if (originalMsg)
        {
            index = chat.msgIndexFromId(msgid);
        }
        else   // message may not have an index yet (not confirmed)
        {
            index = MEGACHAT_INVALID_INDEX;
            originalMsg = findMessageNotConfirmed(chatid, msgid);   // find by transactional id
        }

        if (originalMsg)
        {
            unsigned char newtype = (originalMsg->containMetaSubtype() == Message::ContainsMetaSubType::kRichLink)
                    ? (unsigned char) Message::kMsgNormal
                    : originalMsg->type;

            if (msg && newtype == Message::kMsgNormal)    // actually not deletion, but edit
            {
                // remove ending carrier-returns
                while (msgLen)
                {
                    if (msg[msgLen-1] == '\n' || msg[msgLen-1] == '\r')
                    {
                        msgLen--;
                    }
                    else
                    {
                        break;
                    }
                }
                if (!msgLen)
                {
                    sdkMutex.unlock();
                    return NULL;
                }
            }

            const Message *editedMsg = chatroom->chat().msgModify(*originalMsg, msg, msgLen, NULL, newtype);
            if (editedMsg)
            {
                megaMsg = new MegaChatMessagePrivate(*editedMsg, Message::kSending, index);
            }
        }
    }

    sdkMutex.unlock();
    return megaMsg;
}

MegaChatMessage *MegaChatApiImpl::removeRichLink(MegaChatHandle chatid, MegaChatHandle msgid)
{
    MegaChatMessagePrivate *megaMsg = NULL;
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Message *originalMsg = findMessage(chatid, msgid);
        if (!originalMsg || originalMsg->containMetaSubtype() != Message::ContainsMetaSubType::kRichLink)
        {
            sdkMutex.unlock();
            return NULL;
        }

        uint8_t containsMetaType = originalMsg->containMetaSubtype();
        std::string containsMetaJson = originalMsg->containsMetaJson();
        const MegaChatContainsMeta *containsMeta = JSonUtils::parseContainsMeta(containsMetaJson.c_str(), containsMetaType);
        if (!containsMeta || containsMeta->getType() != MegaChatContainsMeta::CONTAINS_META_RICH_PREVIEW)
        {
            delete containsMeta;
            sdkMutex.unlock();
            return NULL;
        }

        const char *msg = containsMeta->getRichPreview()->getText();
        assert(msg);
        string content = msg ? msg : "";

        const Message *editedMsg = chatroom->chat().removeRichLink(*originalMsg, content);
        if (editedMsg)
        {
            Idx index = chat.msgIndexFromId(msgid);
            megaMsg = new MegaChatMessagePrivate(*editedMsg, Message::kSending, index);
        }

        delete containsMeta;
    }

    sdkMutex.unlock();
    return megaMsg;
}

bool MegaChatApiImpl::setMessageSeen(MegaChatHandle chatid, MegaChatHandle msgid)
{
    bool ret = false;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        ret = chatroom->chat().setMessageSeen((Id) msgid);
    }

    sdkMutex.unlock();

    return ret;
}

MegaChatMessage *MegaChatApiImpl::getLastMessageSeen(MegaChatHandle chatid)
{
    MegaChatMessagePrivate *megaMsg = NULL;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        Idx index = chat.lastSeenIdx();
        if (index != CHATD_IDX_INVALID)
        {
            const Message *msg = chat.findOrNull(index);
            if (msg)
            {
                Message::Status status = chat.getMsgStatus(*msg, index);
                megaMsg = new MegaChatMessagePrivate(*msg, status, index);
            }
        }
    }

    sdkMutex.unlock();

    return megaMsg;
}

MegaChatHandle MegaChatApiImpl::getLastMessageSeenId(MegaChatHandle chatid)
{
    MegaChatHandle lastMessageSeenId = MEGACHAT_INVALID_HANDLE;

    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        lastMessageSeenId = chat.lastSeenId();
    }

    sdkMutex.unlock();

    return lastMessageSeenId;
}

void MegaChatApiImpl::removeUnsentMessage(MegaChatHandle chatid, MegaChatHandle rowid)
{
    sdkMutex.lock();

    ChatRoom *chatroom = findChatRoom(chatid);
    if (chatroom)
    {
        Chat &chat = chatroom->chat();
        chat.removeManualSend(rowid);
    }

    sdkMutex.unlock();
}

void MegaChatApiImpl::sendTypingNotification(MegaChatHandle chatid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SEND_TYPING_NOTIF, listener);
    request->setChatHandle(chatid);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::sendStopTypingNotification(MegaChatHandle chatid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SEND_TYPING_NOTIF, listener);
    request->setChatHandle(chatid);
    request->setFlag(false);
    requestQueue.push(request);
    waiter->notify();
}

bool MegaChatApiImpl::isMessageReceptionConfirmationActive() const
{
    return mClient ? mClient->mChatdClient->isMessageReceivedConfirmationActive() : false;
}

void MegaChatApiImpl::saveCurrentState()
{
    sdkMutex.lock();

    if (mClient && !mTerminating)
    {
        mClient->saveDb();
    }

    sdkMutex.unlock();
}

void MegaChatApiImpl::pushReceived(bool beep, MegaChatHandle chatid, int type, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_PUSH_RECEIVED, listener);
    request->setFlag(beep);
    request->setChatHandle(chatid);
    request->setParamType(type);
    requestQueue.push(request);
    waiter->notify();
}

#ifndef KARERE_DISABLE_WEBRTC

MegaStringList *MegaChatApiImpl::getChatVideoInDevices()
{
    std::set<std::string> devicesVector;
    sdkMutex.lock();
    if (mClient && mClient->rtc)
    {
        mClient->rtc->getVideoInDevices(devicesVector);
    }
    else
    {
        API_LOG_ERROR("Failed to get video-in devices");
    }
    sdkMutex.unlock();

    MegaStringList *devices = getChatInDevices(devicesVector);

    return devices;
}

void MegaChatApiImpl::setChatVideoInDevice(const char *device, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_CHANGE_VIDEO_STREAM, listener);
    request->setText(device);
    requestQueue.push(request);
    waiter->notify();
}

char *MegaChatApiImpl::getVideoDeviceSelected()
{
    char *deviceName = nullptr;
    sdkMutex.lock();
    if (mClient && mClient->rtc)
    {
        deviceName = MegaApi::strdup(mClient->rtc->getVideoDeviceSelected().c_str());
    }
    else
    {
        API_LOG_ERROR("Failed to get selected video-in device");
    }
    sdkMutex.unlock();

    return deviceName;
}

void MegaChatApiImpl::startChatCall(MegaChatHandle chatid, bool enableVideo, bool enableAudio, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_START_CHAT_CALL, listener);
    request->setChatHandle(chatid);
    request->setFlag(enableVideo);
    request->setParamType(enableAudio);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::answerChatCall(MegaChatHandle chatid, bool enableVideo, bool enableAudio, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_ANSWER_CHAT_CALL, listener);
    request->setChatHandle(chatid);
    request->setFlag(enableVideo);
    request->setParamType(enableAudio);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::hangChatCall(MegaChatHandle callid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_HANG_CHAT_CALL, listener);
    request->setChatHandle(callid);
    request->setFlag(false);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::endChatCall(MegaChatHandle callid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_HANG_CHAT_CALL, listener);
    request->setChatHandle(callid);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setAudioEnable(MegaChatHandle chatid, bool enable, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_DISABLE_AUDIO_VIDEO_CALL, listener);
    request->setChatHandle(chatid);
    request->setFlag(enable);
    request->setParamType(MegaChatRequest::AUDIO);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setVideoEnable(MegaChatHandle chatid, bool enable, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_DISABLE_AUDIO_VIDEO_CALL, listener);
    request->setChatHandle(chatid);
    request->setFlag(enable);
    request->setParamType(MegaChatRequest::VIDEO);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::openVideoDevice(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_OPEN_VIDEO_DEVICE, listener);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::releaseVideoDevice(MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_OPEN_VIDEO_DEVICE, listener);
    request->setFlag(false);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::requestHiResQuality(MegaChatHandle chatid, MegaChatHandle clientId, int quality, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REQUEST_HIRES_QUALITY, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(clientId);
    request->setParamType(quality);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::removeSpeaker(MegaChatHandle chatid, MegaChatHandle clientId, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_DEL_SPEAKER, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(clientId);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::setCallOnHold(MegaChatHandle chatid, bool setOnHold, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_SET_CALL_ON_HOLD, listener);
    request->setChatHandle(chatid);
    request->setFlag(setOnHold);
    requestQueue.push(request);
    waiter->notify();
}

bool MegaChatApiImpl::setIgnoredCall(MegaChatHandle chatId)
{
    if (!mClient->rtc)
    {
        API_LOG_ERROR("Ignore call - WebRTC is not initialized");
        return false;
    }

    if (chatId == MEGACHAT_INVALID_HANDLE)
    {
        API_LOG_ERROR("Ignore call - Invalid chatId");
        return false;
    }

    SdkMutexGuard g(sdkMutex);
    rtcModule::ICall* call = mClient->rtc->findCallByChatid(chatId);
    if (!call)
    {
        API_LOG_ERROR("Ignore call - Failed to get the call associated to chat room");
        return false;
    }

    if (call->isIgnored())
    {
        API_LOG_ERROR("Ignore call - Call is already marked as ignored");
        return false;
    }

    call->ignoreCall();
    return true;
}

MegaChatCall *MegaChatApiImpl::getChatCall(MegaChatHandle chatId)
{
    MegaChatCall *chatCall = nullptr;
    if (!mClient->rtc)
    {
        API_LOG_ERROR("MegaChatApiImpl::getChatCall - WebRTC is not initialized");
        return chatCall;
    }

    SdkMutexGuard g(sdkMutex);

    if (chatId != MEGACHAT_INVALID_HANDLE)
    {
        rtcModule::ICall* call = mClient->rtc->findCallByChatid(chatId);
        if (!call)
        {
            API_LOG_ERROR("MegaChatApiImpl::getChatCall - Failed to get the call associated to chat room");
            return chatCall;
        }

        chatCall = new MegaChatCallPrivate(*call);
    }

    return chatCall;
}

MegaChatCall *MegaChatApiImpl::getChatCallByCallId(MegaChatHandle callId)
{
    MegaChatCall *chatCall = NULL;

    sdkMutex.lock();

    MegaHandleList *calls = getChatCalls();
    for (unsigned int i = 0; i < calls->size(); i++)
    {
        karere::Id chatId = calls->get(i);
        MegaChatCall *call = getChatCall(chatId);
        if (call && call->getCallId() == callId)
        {
            chatCall =  call;
            break;
        }
        else
        {
            delete call;
        }
    }

    delete calls;

    sdkMutex.unlock();
    return chatCall;
}

int MegaChatApiImpl::getNumCalls()
{
    int numCalls = 0;
    sdkMutex.lock();
    if (mClient && mClient->rtc)
    {
        numCalls = mClient->rtc->getNumCalls();
    }
    sdkMutex.unlock();

    return numCalls;
}

MegaHandleList *MegaChatApiImpl::getChatCalls(int callState)
{
    MegaHandleListPrivate *callList = new MegaHandleListPrivate();

    sdkMutex.lock();
    if (mClient && mClient->rtc)
    {
        std::vector<karere::Id> chatids = mClient->rtc->chatsWithCall();
        for (unsigned int i = 0; i < chatids.size(); i++)
        {
            rtcModule::ICall* call = mClient->rtc->findCallByChatid(chatids[i]);
            if (call && (callState == -1 || call->getState() == callState))
            {
                callList->addMegaHandle(chatids[i]);
            }
        }
    }

    sdkMutex.unlock();
    return callList;
}

MegaHandleList *MegaChatApiImpl::getChatCallsIds()
{
    MegaHandleListPrivate *callList = new MegaHandleListPrivate();

    sdkMutex.lock();

    MegaHandleList *chatids = getChatCalls();
    for (unsigned int i = 0; i < chatids->size(); i++)
    {
        karere::Id chatId = chatids->get(i);
        MegaChatCall *call = getChatCall(chatId);
        if (call)
        {
            callList->addMegaHandle(call->getCallId());
            delete call;
        }
    }

    delete chatids;

    sdkMutex.unlock();
    return callList;
}

bool MegaChatApiImpl::hasCallInChatRoom(MegaChatHandle chatid)
{
    bool hasCall = false;
    sdkMutex.lock();

    if (mClient && mClient->rtc)
    {
        hasCall = mClient->rtc->findCallByChatid(chatid);
    }

    sdkMutex.unlock();
    return hasCall;
}

void MegaChatApiImpl::addChatCallListener(MegaChatCallListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    callListeners.insert(listener);
    sdkMutex.unlock();
}

int MegaChatApiImpl::getMaxCallParticipants()
{
    return rtcModule::RtcConstant::kMaxCallReceivers;
}

int MegaChatApiImpl::getMaxVideoCallParticipants()
{
    return rtcModule::RtcConstant::kMaxCallVideoSenders;
}

bool MegaChatApiImpl::isAudioLevelMonitorEnabled(MegaChatHandle chatid)
{
    if (chatid == MEGACHAT_INVALID_HANDLE)
    {
        API_LOG_ERROR("isAudioLevelMonitorEnabled - Invalid chatId");
        return false;
    }

    SdkMutexGuard g(sdkMutex);
    rtcModule::ICall *call = findCall(chatid);
    if (!call)
    {
       API_LOG_ERROR("isAudioLevelMonitorEnabled - Failed to get the call associated to chat room");
       return false;
    }

    return call->isAudioLevelMonitorEnabled();
}

void MegaChatApiImpl::enableAudioLevelMonitor(bool enable, MegaChatHandle chatid, MegaChatRequestListener* listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_ENABLE_AUDIO_LEVEL_MONITOR, listener);
    request->setChatHandle(chatid);
    request->setFlag(enable);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::requestSpeak(MegaChatHandle chatid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REQUEST_SPEAK, listener);
    request->setChatHandle(chatid);
    request->setFlag(true);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::removeRequestSpeak(MegaChatHandle chatid, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REQUEST_SPEAK, listener);
    request->setChatHandle(chatid);
    request->setFlag(false);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::approveSpeakRequest(MegaChatHandle chatid, MegaChatHandle clientId, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_APPROVE_SPEAK, listener);
    request->setChatHandle(chatid);
    request->setFlag(true);
    request->setUserHandle(clientId);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::rejectSpeakRequest(MegaChatHandle chatid, MegaChatHandle clientId, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_APPROVE_SPEAK, listener);
    request->setChatHandle(chatid);
    request->setFlag(false);
    request->setUserHandle(clientId);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::requestHiResVideo(MegaChatHandle chatid, MegaChatHandle clientId, int quality, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REQUEST_HIGH_RES_VIDEO, listener);
    request->setChatHandle(chatid);
    request->setFlag(true);
    request->setUserHandle(clientId);
    request->setPrivilege(quality);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::stopHiResVideo(MegaChatHandle chatid, MegaHandleList *clientIds, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REQUEST_HIGH_RES_VIDEO, listener);
    request->setChatHandle(chatid);
    request->setFlag(false);
    request->setMegaHandleList(clientIds);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::requestLowResVideo(MegaChatHandle chatid, MegaHandleList *clientIds, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REQUEST_LOW_RES_VIDEO, listener);
    request->setChatHandle(chatid);
    request->setFlag(true);
    request->setMegaHandleList(clientIds);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::stopLowResVideo(MegaChatHandle chatid, MegaHandleList *clientIds, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_REQUEST_LOW_RES_VIDEO, listener);
    request->setChatHandle(chatid);
    request->setFlag(false);
    request->setMegaHandleList(clientIds);
    requestQueue.push(request);
    waiter->notify();
}

void MegaChatApiImpl::onNewCall(rtcModule::ICall &call)
{
    call.setCallHandler(new MegaChatCallHandler(this)); // takes ownership
}

void MegaChatApiImpl::onAddPeer(rtcModule::ICall &call, Id peer)
{
    std::unique_ptr<MegaChatCallPrivate> chatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    chatCall->setPeerid(peer, true);
    fireOnChatCallUpdate(chatCall.get());
}

void MegaChatApiImpl::onRemovePeer(rtcModule::ICall &call, Id peer)
{
    std::unique_ptr<MegaChatCallPrivate> chatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    chatCall->setPeerid(peer, false);
    fireOnChatCallUpdate(chatCall.get());
}

#endif

void MegaChatApiImpl::addChatRequestListener(MegaChatRequestListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    requestListeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatListener(MegaChatListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    listeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatRoomListener(MegaChatHandle chatid, MegaChatRoomListener *listener)
{
    if (!listener || chatid == MEGACHAT_INVALID_HANDLE)
    {
        return;
    }

    sdkMutex.lock();
    MegaChatRoomHandler *roomHandler = getChatRoomHandler(chatid);
    roomHandler->addChatRoomListener(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatNotificationListener(MegaChatNotificationListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    notificationListeners.insert(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatRequestListener(MegaChatRequestListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    requestListeners.erase(listener);

    map<int,MegaChatRequestPrivate*>::iterator it = requestMap.begin();
    while (it != requestMap.end())
    {
        MegaChatRequestPrivate* request = it->second;
        if(request->getListener() == listener)
        {
            request->setListener(NULL);
        }

        it++;
    }

    requestQueue.removeListener(listener);
    sdkMutex.unlock();
}

#ifndef KARERE_DISABLE_WEBRTC

void MegaChatApiImpl::removeChatCallListener(MegaChatCallListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    callListeners.erase(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::addChatVideoListener(MegaChatHandle chatid, MegaChatHandle clientId, rtcModule::VideoResolution videoResolution, MegaChatVideoListener *listener)
{
    if (!listener)
    {
        return;
    }

    assert(videoResolution != rtcModule::VideoResolution::kUndefined);
    videoMutex.lock();
    if (clientId == 0)
    {
        mLocalVideoListeners[chatid].insert(listener);
        marshallCall([this, chatid]()
        {
            // avoid access from App thread to RtcModule::mRenderers
            if (mClient && mClient->rtc)
            {
                mClient->rtc->addLocalVideoRenderer(chatid, new MegaChatVideoReceiver(this, chatid, rtcModule::VideoResolution::kHiRes));
            }

        }, this);
    }
    else if (videoResolution == rtcModule::VideoResolution::kHiRes)
    {
        mVideoListenersHiRes[chatid][clientId].insert(listener);
    }
    else if (videoResolution == rtcModule::VideoResolution::kLowRes)
    {
        mVideoListenersLowRes[chatid][clientId].insert(listener);
    }

    videoMutex.unlock();
}

void MegaChatApiImpl::removeChatVideoListener(MegaChatHandle chatid, MegaChatHandle clientId, rtcModule::VideoResolution videoResolution, MegaChatVideoListener *listener)
{
    if (!listener)
    {
        return;
    }

    assert(videoResolution != rtcModule::VideoResolution::kUndefined);
    videoMutex.lock();
    if (clientId == 0)
    {
        auto it = mLocalVideoListeners.find(chatid);
        if (it != mLocalVideoListeners.end())
        {
            MegaChatVideoListener_set &videoListenersSet = it->second;
            videoListenersSet.erase(listener);
            if (videoListenersSet.empty())
            {
                // if videoListenersSet is empty, remove entry from mLocalVideoListeners map
                mLocalVideoListeners.erase(chatid);
                marshallCall([this, chatid]()
                {
                    // avoid access from App thread to RtcModule::mRenderers
                    if (mClient && mClient->rtc)
                    {
                        mClient->rtc->removeLocalVideoRenderer(chatid);
                    }
                }, this);
            }
        }
    }
    else if (videoResolution == rtcModule::VideoResolution::kHiRes)
    {
        auto itHiRes = mVideoListenersHiRes.find(chatid);
        if (itHiRes != mVideoListenersHiRes.end())
        {
            MegaChatPeerVideoListener_map &videoListenersMap = itHiRes->second;
            auto auxit = videoListenersMap.find(clientId);
            if (auxit != videoListenersMap.end())
            {
                // remove listener from MegaChatVideoListener_set
                MegaChatVideoListener_set &videoListener_set = auxit->second;
                videoListener_set.erase(listener);
                if (videoListener_set.empty())
                {
                    // if MegaChatVideoListener_set is empty, remove entry from MegaChatPeerVideoListener_map
                    videoListenersMap.erase(clientId);
                }
            }

            if (videoListenersMap.empty())
            {
                // if MegaChatPeerVideoListener_map is empty, remove entry from mVideoListenersHiRes map
                mVideoListenersHiRes.erase(chatid);
            }
        }
    }
    else if (videoResolution == rtcModule::VideoResolution::kLowRes)
    {
        assert(clientId); // local video listeners can't be un/registered into this map
        auto itLowRes = mVideoListenersLowRes.find(chatid);
        if (itLowRes != mVideoListenersLowRes.end())
        {
            MegaChatPeerVideoListener_map &videoListenersMap = itLowRes->second;
            auto auxit = videoListenersMap.find(clientId);
            if (auxit != videoListenersMap.end())
            {
                // remove listener from MegaChatVideoListener_set
                MegaChatVideoListener_set &videoListener_set = auxit->second;
                videoListener_set.erase(listener);
                if (videoListener_set.empty())
                {
                    // if MegaChatVideoListener_set is empty, remove entry from MegaChatPeerVideoListener_map
                    videoListenersMap.erase(clientId);
                }
            }

            if (videoListenersMap.empty())
            {
                // if MegaChatPeerVideoListener_map is empty, remove entry from mVideoListenersLowRes map
                mVideoListenersLowRes.erase(chatid);
            }
        }
    }
    videoMutex.unlock();
}

#endif  // webrtc

void MegaChatApiImpl::removeChatListener(MegaChatListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    listeners.erase(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatRoomListener(MegaChatHandle chatid, MegaChatRoomListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    MegaChatRoomHandler *roomHandler = getChatRoomHandler(chatid);
    roomHandler->removeChatRoomListener(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::removeChatNotificationListener(MegaChatNotificationListener *listener)
{
    if (!listener)
    {
        return;
    }

    sdkMutex.lock();
    notificationListeners.erase(listener);
    sdkMutex.unlock();
}

void MegaChatApiImpl::manageReaction(MegaChatHandle chatid, MegaChatHandle msgid, const char *reaction, bool add, MegaChatRequestListener *listener)
{
    MegaChatRequestPrivate *request = new MegaChatRequestPrivate(MegaChatRequest::TYPE_MANAGE_REACTION, listener);
    request->setChatHandle(chatid);
    request->setUserHandle(msgid);
    request->setText(reaction);
    request->setFlag(add);
    requestQueue.push(request);
    waiter->notify();
}

int MegaChatApiImpl::getMessageReactionCount(MegaChatHandle chatid, MegaChatHandle msgid, const char *reaction)
{
    if (!reaction)
    {
        return -1;
    }

    SdkMutexGuard g(sdkMutex);
    Message *msg = findMessage(chatid, msgid);
    if (!msg)
    {
        API_LOG_ERROR("Chatroom or message not found");
        return -1;
    }

    // Update users of confirmed reactions with pending reactions
    int count = msg->getReactionCount(reaction);
    ChatRoom *chatroom = findChatRoom(chatid);
    auto pendingReactions = chatroom->chat().getPendingReactions();
    for (auto &auxReact : pendingReactions)
    {
        if (auxReact.mMsgId == msgid && auxReact.mReactionString == reaction)
        {
            (auxReact.mStatus == OP_ADDREACTION)
                ? count++
                : count--;

            break;
        }
    }

    return count;
}

MegaStringList* MegaChatApiImpl::getMessageReactions(MegaChatHandle chatid, MegaChatHandle msgid)
{
    SdkMutexGuard g(sdkMutex);
    Message *msg = findMessage(chatid, msgid);
    if (!msg)
    {
        API_LOG_ERROR("Chatroom or message not found");
        return new MegaStringListPrivate();
    }

    string_vector reactions;
    const std::vector<Message::Reaction> &confirmedReactions = msg->getReactions();
    const Chat::PendingReactions& pendingReactions = (findChatRoom(chatid))->chat().getPendingReactions();

    // iterate through confirmed reactions list
    for (auto &auxReact : confirmedReactions)
    {
         int reactUsers = static_cast<int>(auxReact.mUsers.size());
         for (auto &pendingReact : pendingReactions)
         {
             if (pendingReact.mMsgId == msgid
                     && !pendingReact.mReactionString.compare(auxReact.mReaction))
             {
                // increment or decrement reactUsers, for the confirmed reaction we are checking
                (pendingReact.mStatus == OP_ADDREACTION)
                        ? reactUsers++
                        : reactUsers--;

                // a confirmed reaction only can have one pending reaction
                break;
             }
         }

         if (reactUsers > 0)
         {
             reactions.emplace_back(auxReact.mReaction);
         }
    }

    // add pending reactions that are not on confirmed list
    for (auto &pendingReact : pendingReactions)
    {
        if (pendingReact.mMsgId == msgid
                && !msg->getReactionCount(pendingReact.mReactionString)
                && pendingReact.mStatus == OP_ADDREACTION)
        {
            reactions.emplace_back(pendingReact.mReactionString);
        }
    }

    return new MegaStringListPrivate(move(reactions));
}

MegaHandleList* MegaChatApiImpl::getReactionUsers(MegaChatHandle chatid, MegaChatHandle msgid, const char *reaction)
{
    MegaHandleList *userList = MegaHandleList::createInstance();
    if (!reaction)
    {
        return userList;
    }

    SdkMutexGuard g(sdkMutex);
    Message *msg = findMessage(chatid, msgid);
    ChatRoom *chatroom = findChatRoom(chatid);
    if (!msg || !chatroom)
    {
        API_LOG_ERROR("Chatroom or message not found");
        return userList;
    }

    bool reacted = false;
    string reactionStr(reaction);
    int pendingReactionStatus = chatroom->chat().getPendingReactionStatus(reactionStr, msgid);
    const std::vector<karere::Id> &users = msg->getReactionUsers(reactionStr);
    for (auto user: users)
    {
        if (user != mClient->myHandle())
        {
            userList->addMegaHandle(user);
        }
        else
        {
            if (pendingReactionStatus != OP_DELREACTION)
            {
                // if we have reacted and there's no a pending DELREACTION
                reacted = true;
                userList->addMegaHandle(mClient->myHandle());
            }
        }
    }

    if (!reacted && pendingReactionStatus == OP_ADDREACTION)
    {
        // if we don't have reacted and there's a pending ADDREACTION
        userList->addMegaHandle(mClient->myHandle());
    }

    return userList;
}

void MegaChatApiImpl::setPublicKeyPinning(bool enable)
{
    SdkMutexGuard g(sdkMutex);
    ::WebsocketsClient::publicKeyPinning = enable;
}

IApp::IChatHandler *MegaChatApiImpl::createChatHandler(ChatRoom &room)
{
    return getChatRoomHandler(room.chatid());
}

IApp::IChatListHandler *MegaChatApiImpl::chatListHandler()
{
    return this;
}

#ifndef KARERE_DISABLE_WEBRTC

MegaStringList *MegaChatApiImpl::getChatInDevices(const std::set<string> &devices)
{
    string_vector buffer;
    for (const std::string &device : devices)
    {
        buffer.push_back(device);
    }

    return new MegaStringListPrivate(move(buffer));
}

void MegaChatApiImpl::cleanCalls()
{
    sdkMutex.lock();

    if (mClient && mClient->rtc)
    {
        std::vector<karere::Id> chatids = mClient->rtc->chatsWithCall();
        for (unsigned int i = 0; i < chatids.size(); i++)
        {
            mClient->rtc->removeCall(chatids[i]);
        }
    }

    sdkMutex.unlock();
}

rtcModule::ICall *MegaChatApiImpl::findCall(MegaChatHandle chatid)
{
    SdkMutexGuard g(sdkMutex);
    if (mClient && mClient->rtc)
    {
       return mClient->rtc->findCallByChatid(chatid);
    }

    return nullptr;

}

#endif

void MegaChatApiImpl::cleanChatHandlers()
{
#ifndef KARERE_DISABLE_WEBRTC
    cleanCalls();
#endif

	MegaChatHandle chatid;
	for (auto it = chatRoomHandler.begin(); it != chatRoomHandler.end();)
	{
		chatid = it->first;
		it++;

		closeChatRoom(chatid, NULL);
	}
	assert(chatRoomHandler.empty());

	for (auto it = nodeHistoryHandlers.begin(); it != nodeHistoryHandlers.end();)
	{
		chatid = it->first;
		it++;

		closeNodeHistory(chatid, NULL);
	}
	assert(nodeHistoryHandlers.empty());
}

void MegaChatApiImpl::onInitStateChange(int newState)
{
    API_LOG_DEBUG("Karere initialization state has changed: %d", newState);

    if (newState == karere::Client::kInitErrSidInvalid)
    {
        API_LOG_WARNING("Invalid session detected (API_ESID). Logging out...");
        logout();
        return;
    }

    int state = MegaChatApiImpl::convertInitState(newState);

    // only notify meaningful state to the app
    if (state == MegaChatApi::INIT_ERROR ||
            state == MegaChatApi::INIT_WAITING_NEW_SESSION ||
            state == MegaChatApi::INIT_OFFLINE_SESSION ||
            state == MegaChatApi::INIT_ONLINE_SESSION ||
            state == MegaChatApi::INIT_NO_CACHE)
    {
        fireOnChatInitStateUpdate(state);
    }
}

void MegaChatApiImpl::onChatNotification(karere::Id chatid, const Message &msg, Message::Status status, Idx idx)
{
    if (mMegaApi->isChatNotifiable(chatid)   // filtering based on push-notification settings
            && !msg.isEncrypted())          // avoid msgs to be notified when marked as "seen", but still decrypting
    {
         MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
         fireOnChatNotification(chatid, message);
     }
}

int MegaChatApiImpl::convertInitState(int state)
{
    switch (state)
    {
    case karere::Client::kInitErrGeneric:
    case karere::Client::kInitErrCorruptCache:
    case karere::Client::kInitErrSidMismatch:
    case karere::Client::kInitErrAlready:
        return MegaChatApi::INIT_ERROR;

    case karere::Client::kInitCreated:
        return MegaChatApi::INIT_NOT_DONE;

    case karere::Client::kInitErrNoCache:
        return MegaChatApi::INIT_NO_CACHE;

    case karere::Client::kInitWaitingNewSession:
        return MegaChatApi::INIT_WAITING_NEW_SESSION;

    case karere::Client::kInitHasOfflineSession:
        return MegaChatApi::INIT_OFFLINE_SESSION;

    case karere::Client::kInitHasOnlineSession:
        return MegaChatApi::INIT_ONLINE_SESSION;

    case karere::Client::kInitAnonymousMode:
        return MegaChatApi::INIT_ANONYMOUS;

    case karere::Client::kInitTerminated:
    case karere::Client::kInitErrSidInvalid:
    default:
        return state;
    }
}

int MegaChatApiImpl::convertChatConnectionState(ChatState state)
{
    switch(state)
    {
    case ChatState::kChatStateOffline:
        return MegaChatApi::CHAT_CONNECTION_OFFLINE;
    case ChatState::kChatStateConnecting:
        return MegaChatApi::CHAT_CONNECTION_IN_PROGRESS;
    case ChatState::kChatStateJoining:
        return MegaChatApi::CHAT_CONNECTION_LOGGING;
    case ChatState::kChatStateOnline:
        return MegaChatApi::CHAT_CONNECTION_ONLINE;
    }

    assert(false);  // check compilation warnings, new ChatState not considered
    return state;
}

IApp::IGroupChatListItem *MegaChatApiImpl::addGroupChatItem(GroupChatRoom &chat)
{
    MegaChatGroupListItemHandler *itemHandler = new MegaChatGroupListItemHandler(*this, chat);
    chatGroupListItemHandler.insert(itemHandler);

    // notify the app about the new chatroom
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(chat);
    fireOnChatListItemUpdate(item);

    return (IGroupChatListItem *) itemHandler;
}

IApp::IPeerChatListItem *MegaChatApiImpl::addPeerChatItem(PeerChatRoom &chat)
{
    MegaChatPeerListItemHandler *itemHandler = new MegaChatPeerListItemHandler(*this, chat);
    chatPeerListItemHandler.insert(itemHandler);

    // notify the app about the new chatroom
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(chat);
    fireOnChatListItemUpdate(item);

    return (IPeerChatListItem *) itemHandler;
}

void MegaChatApiImpl::removeGroupChatItem(IGroupChatListItem &item)
{
    set<MegaChatGroupListItemHandler *>::iterator it = chatGroupListItemHandler.begin();
    while (it != chatGroupListItemHandler.end())
    {
        IGroupChatListItem *itemHandler = (*it);
        if (itemHandler == &item)
        {
            delete itemHandler;
            chatGroupListItemHandler.erase(it);
            return;
        }

        it++;
    }
}

void MegaChatApiImpl::removePeerChatItem(IPeerChatListItem &item)
{
    set<MegaChatPeerListItemHandler *>::iterator it = chatPeerListItemHandler.begin();
    while (it != chatPeerListItemHandler.end())
    {
        IPeerChatListItem *itemHandler = (*it);
        if (itemHandler == &item)
        {
            delete (itemHandler);
            chatPeerListItemHandler.erase(it);
            return;
        }

        it++;
    }
}

void MegaChatApiImpl::onPresenceChanged(Id userid, Presence pres, bool inProgress)
{
    if (inProgress)
    {
        API_LOG_INFO("My own presence is being changed to %s", pres.toString());
    }
    else
    {
        API_LOG_INFO("Presence of user %s has been changed to %s", ID_CSTR(userid), pres.toString());
    }
    fireOnChatOnlineStatusUpdate(userid.val, pres.status(), inProgress);
}

void MegaChatApiImpl::onPresenceConfigChanged(const presenced::Config &state, bool pending)
{
    MegaChatPresenceConfigPrivate *config = new MegaChatPresenceConfigPrivate(state, pending);
    fireOnChatPresenceConfigUpdate(config);
}

void MegaChatApiImpl::onPresenceLastGreenUpdated(Id userid, uint16_t lastGreen)
{
    fireOnChatPresenceLastGreenUpdated(userid, lastGreen);
}

void ChatRequestQueue::push(MegaChatRequestPrivate *request)
{
    mutex.lock();
    requests.push_back(request);
    mutex.unlock();
}

void ChatRequestQueue::push_front(MegaChatRequestPrivate *request)
{
    mutex.lock();
    requests.push_front(request);
    mutex.unlock();
}

MegaChatRequestPrivate *ChatRequestQueue::pop()
{
    mutex.lock();
    if(requests.empty())
    {
        mutex.unlock();
        return NULL;
    }
    MegaChatRequestPrivate *request = requests.front();
    requests.pop_front();
    mutex.unlock();
    return request;
}

void ChatRequestQueue::removeListener(MegaChatRequestListener *listener)
{
    mutex.lock();

    deque<MegaChatRequestPrivate *>::iterator it = requests.begin();
    while(it != requests.end())
    {
        MegaChatRequestPrivate *request = (*it);
        if(request->getListener()==listener)
            request->setListener(NULL);
        it++;
    }

    mutex.unlock();
}

void EventQueue::push(megaMessage* transfer)
{
    mutex.lock();
    events.push_back(transfer);
    mutex.unlock();
}

void EventQueue::push_front(megaMessage* event)
{
    mutex.lock();
    events.push_front(event);
    mutex.unlock();
}

megaMessage* EventQueue::pop()
{
    mutex.lock();
    if(events.empty())
    {
        mutex.unlock();
        return NULL;
    }

    megaMessage* event = events.front();
    events.pop_front();
    mutex.unlock();
    return event;
}

bool EventQueue::isEmpty()
{
    bool ret;

    mutex.lock();
    ret = events.empty();
    mutex.unlock();

    return ret;
}

size_t EventQueue::size()
{
    size_t ret;

    mutex.lock();
    ret = events.size();
    mutex.unlock();

    return ret;
}

MegaChatRequestPrivate::MegaChatRequestPrivate(int type, MegaChatRequestListener *listener)
{
    mType = type;
    mTag = 0;
    mListener = listener;

    mNumber = 0;
    mRetry = 0;
    mFlag = false;
    mPeerList = NULL;
    mChatid = MEGACHAT_INVALID_HANDLE;
    mUserHandle = MEGACHAT_INVALID_HANDLE;
    mPrivilege = MegaChatPeerList::PRIV_UNKNOWN;
    mText = NULL;
    mLink = NULL;
    mMessage = NULL;
    mMegaNodeList = NULL;
    mMegaHandleList = NULL;
    mParamType = 0;
}

MegaChatRequestPrivate::MegaChatRequestPrivate(MegaChatRequestPrivate &request)
{
    mText = NULL;
    mPeerList = NULL;
    mMessage = NULL;
    mMegaNodeList = NULL;
    mMegaHandleList = NULL;
    mLink = NULL;

    mType = request.getType();
    mListener = request.getListener();
    setTag(request.getTag());
    setNumber(request.getNumber());
    setNumRetry(request.getNumRetry());
    setFlag(request.getFlag());
    setMegaChatPeerList(request.getMegaChatPeerList());
    setChatHandle(request.getChatHandle());
    setUserHandle(request.getUserHandle());
    setPrivilege(request.getPrivilege());
    setText(request.getText());
    setLink(request.getLink());
    setMegaChatMessage(request.getMegaChatMessage());
    setMegaNodeList(request.getMegaNodeList());
    setMegaHandleList(request.getMegaHandleList());
    if (mMegaHandleList)
    {
        for (unsigned int i = 0; i < mMegaHandleList->size(); i++)
        {
            MegaChatHandle chatid = mMegaHandleList->get(i);
            setMegaHandleListByChat(chatid, request.getMegaHandleListByChat(chatid));
        }
    }

    setParamType(request.getParamType());
}

MegaChatRequestPrivate::~MegaChatRequestPrivate()
{
    delete mPeerList;
    delete [] mText;
    delete [] mLink;
    delete mMessage;
    delete mMegaNodeList;
    delete mMegaHandleList;
    for (map<MegaChatHandle, MegaHandleList*>::iterator it = mMegaHandleListMap.begin(); it != mMegaHandleListMap.end(); it++)
    {
        delete it->second;
    }
}

MegaChatRequest *MegaChatRequestPrivate::copy()
{
    return new MegaChatRequestPrivate(*this);
}

const char *MegaChatRequestPrivate::getRequestString() const
{
    switch(mType)
    {
        case TYPE_DELETE: return "DELETE";
        case TYPE_LOGOUT: return "LOGOUT";
        case TYPE_CONNECT: return "CONNECT";
        case TYPE_INITIALIZE: return "INITIALIZE";
        case TYPE_SET_ONLINE_STATUS: return "SET_CHAT_STATUS";
        case TYPE_CREATE_CHATROOM: return "CREATE CHATROOM";
        case TYPE_INVITE_TO_CHATROOM: return "INVITE_TO_CHATROOM";
        case TYPE_REMOVE_FROM_CHATROOM: return "REMOVE_FROM_CHATROOM";
        case TYPE_UPDATE_PEER_PERMISSIONS: return "UPDATE_PEER_PERMISSIONS";
        case TYPE_TRUNCATE_HISTORY: return "TRUNCATE_HISTORY";
        case TYPE_EDIT_CHATROOM_NAME: return "EDIT_CHATROOM_NAME";
        case TYPE_EDIT_CHATROOM_PIC: return "EDIT_CHATROOM_PIC";
        case TYPE_GET_FIRSTNAME: return "GET_FIRSTNAME";
        case TYPE_GET_LASTNAME: return "GET_LASTNAME";
        case TYPE_GET_EMAIL: return "GET_EMAIL";
        case TYPE_DISCONNECT: return "DISCONNECT";
        case TYPE_SET_BACKGROUND_STATUS: return "SET_BACKGROUND_STATUS";
        case TYPE_RETRY_PENDING_CONNECTIONS: return "RETRY_PENDING_CONNECTIONS";
        case TYPE_START_CHAT_CALL: return "START_CHAT_CALL";
        case TYPE_ANSWER_CHAT_CALL: return "ANSWER_CHAT_CALL";
        case TYPE_DISABLE_AUDIO_VIDEO_CALL: return "DISABLE_AUDIO_VIDEO_CALL";
        case TYPE_HANG_CHAT_CALL: return "HANG_CHAT_CALL";
        case TYPE_LOAD_AUDIO_VIDEO_DEVICES: return "LOAD_AUDIO_VIDEO_DEVICES";
        case TYPE_ATTACH_NODE_MESSAGE: return "ATTACH_NODE_MESSAGE";
        case TYPE_REVOKE_NODE_MESSAGE: return "REVOKE_NODE_MESSAGE";
        case TYPE_SHARE_CONTACT: return "SHARE_CONTACT";
        case TYPE_SEND_TYPING_NOTIF: return "SEND_TYPING_NOTIF";
        case TYPE_SIGNAL_ACTIVITY: return "SIGNAL_ACTIVITY";
        case TYPE_SET_PRESENCE_PERSIST: return "SET_PRESENCE_PERSIST";
        case TYPE_SET_PRESENCE_AUTOAWAY: return "SET_PRESENCE_AUTOAWAY";
        case TYPE_ARCHIVE_CHATROOM: return "ARCHIVE_CHATROOM";
        case TYPE_PUSH_RECEIVED: return "PUSH_RECEIVED";
        case TYPE_LOAD_PREVIEW: return "LOAD_PREVIEW";
        case TYPE_CHAT_LINK_HANDLE: return "CHAT_LINK_HANDLE";
        case TYPE_SET_PRIVATE_MODE: return "SET_PRIVATE_MODE";
        case TYPE_AUTOJOIN_PUBLIC_CHAT: return "AUTOJOIN_PUBLIC_CHAT";
        case TYPE_SET_LAST_GREEN_VISIBLE: return "SET_LAST_GREEN_VISIBLE";
        case TYPE_LAST_GREEN: return "LAST_GREEN";
        case TYPE_CHANGE_VIDEO_STREAM: return "CHANGE_VIDEO_STREAM";
        case TYPE_GET_PEER_ATTRIBUTES: return "GET_PEER_ATTRIBUTES";
        case TYPE_IMPORT_MESSAGES: return "IMPORT_MESSAGES";
        case TYPE_SET_RETENTION_TIME: return "SET_RETENTION_TIME";
        case TYPE_SET_CALL_ON_HOLD: return "SET_CALL_ON_HOLD";
        case TYPE_ENABLE_AUDIO_LEVEL_MONITOR: return "ENABLE_AUDIO_LEVEL_MONITOR";
        case TYPE_MANAGE_REACTION: return "MANAGE_REACTION";
        case TYPE_REQUEST_SPEAK: return "REQUEST_SPEAK";
        case TYPE_APPROVE_SPEAK: return "APPROVE_SPEAK";
        case TYPE_REQUEST_HIGH_RES_VIDEO: return "REQUEST_HIGH_RES_VIDEO";
        case TYPE_REQUEST_LOW_RES_VIDEO: return "REQUEST_LOW_RES_VIDEO";
        case TYPE_OPEN_VIDEO_DEVICE: return "OPEN_VIDEO_DEVICE";
        case TYPE_REQUEST_HIRES_QUALITY: return "REQUEST_HIRES_QUALITY";
        case TYPE_DEL_SPEAKER: return "DEL_SPEAKER";
        case TYPE_REQUEST_SVC_LAYERS: return "SVC_LAYERS";
    }
    return "UNKNOWN";
}

const char *MegaChatRequestPrivate::toString() const
{
    return getRequestString();
}

MegaChatRequestListener *MegaChatRequestPrivate::getListener() const
{
    return mListener;
}

int MegaChatRequestPrivate::getType() const
{
    return mType;
}

long long MegaChatRequestPrivate::getNumber() const
{
    return mNumber;
}

int MegaChatRequestPrivate::getNumRetry() const
{
    return mRetry;
}

bool MegaChatRequestPrivate::getFlag() const
{
    return mFlag;
}

MegaChatPeerList *MegaChatRequestPrivate::getMegaChatPeerList()
{
    return mPeerList;
}

MegaChatHandle MegaChatRequestPrivate::getChatHandle()
{
    return mChatid;
}

MegaChatHandle MegaChatRequestPrivate::getUserHandle()
{
    return mUserHandle;
}

int MegaChatRequestPrivate::getPrivilege()
{
    return mPrivilege;
}

const char *MegaChatRequestPrivate::getText() const
{
    return mText;
}

const char *MegaChatRequestPrivate::getLink() const
{
    return mLink;
}

MegaChatMessage *MegaChatRequestPrivate::getMegaChatMessage()
{
    return mMessage;
}

int MegaChatRequestPrivate::getTag() const
{
    return mTag;
}

void MegaChatRequestPrivate::setListener(MegaChatRequestListener *listener)
{
    mListener = listener;
}

void MegaChatRequestPrivate::setTag(int tag)
{
    mTag = tag;
}

void MegaChatRequestPrivate::setNumber(long long number)
{
    mNumber = number;
}

void MegaChatRequestPrivate::setNumRetry(int retry)
{
    mRetry = retry;
}

void MegaChatRequestPrivate::setFlag(bool flag)
{
    mFlag = flag;
}

void MegaChatRequestPrivate::setMegaChatPeerList(MegaChatPeerList *peerList)
{
    if (mPeerList)
        delete mPeerList;

    mPeerList = peerList ? peerList->copy() : NULL;
}

void MegaChatRequestPrivate::setChatHandle(MegaChatHandle chatid)
{
    mChatid = chatid;
}

void MegaChatRequestPrivate::setUserHandle(MegaChatHandle userhandle)
{
    mUserHandle = userhandle;
}

void MegaChatRequestPrivate::setPrivilege(int priv)
{
    mPrivilege = priv;
}

void MegaChatRequestPrivate::setLink(const char *link)
{
    if(mLink)
    {
        delete [] mLink;
    }
    mLink = MegaApi::strdup(link);
}

void MegaChatRequestPrivate::setText(const char *text)
{
    if(mText)
    {
        delete [] mText;
    }
    mText = MegaApi::strdup(text);
}

void MegaChatRequestPrivate::setMegaChatMessage(MegaChatMessage *message)
{
    if (mMessage != NULL)
    {
        delete mMessage;
    }

    mMessage = message ? message->copy() : NULL;
}

void MegaChatRequestPrivate::setMegaHandleList(MegaHandleList *handlelist)
{
    if (mMegaHandleList != NULL)
    {
        delete mMegaHandleList;
    }

    mMegaHandleList = handlelist ? handlelist->copy() : NULL;
}

void MegaChatRequestPrivate::setMegaHandleListByChat(MegaChatHandle chatid, MegaHandleList *handlelist)
{
    MegaHandleList *list = getMegaHandleListByChat(chatid);
    if (list)
    {
        delete list;
    }

    mMegaHandleListMap[chatid] = handlelist ? handlelist->copy() : NULL;
}

MegaNodeList *MegaChatRequestPrivate::getMegaNodeList()
{
    return mMegaNodeList;
}

MegaHandleList *MegaChatRequestPrivate::getMegaHandleListByChat(MegaChatHandle chatid)
{
    map<MegaChatHandle, MegaHandleList*>::iterator it = mMegaHandleListMap.find(chatid);
    if (it != mMegaHandleListMap.end())
    {
        return it->second;
    }

    return NULL;
}

MegaHandleList *MegaChatRequestPrivate::getMegaHandleList()
{
    return mMegaHandleList;
}

int MegaChatRequestPrivate::getParamType()
{
    return mParamType;
}

void MegaChatRequestPrivate::setMegaNodeList(MegaNodeList *nodelist)
{
    if (mMegaNodeList != NULL)
    {
        delete mMegaNodeList;
    }

    mMegaNodeList = nodelist ? nodelist->copy() : NULL;
}

void MegaChatRequestPrivate::setParamType(int paramType)
{
    mParamType = paramType;
}

#ifndef KARERE_DISABLE_WEBRTC

MegaChatSessionPrivate::MegaChatSessionPrivate(const rtcModule::ISession &session)
    : mState(session.getState())
    , mPeerId(session.getPeerid())
    , mClientId(session.getClientid())
    , mAvFlags(session.getAvFlags())
    , mChanged(CHANGE_TYPE_NO_CHANGES)
    , mHasRequestSpeak(session.hasRequestSpeak())
    , mAudioDetected(session.isAudioDetected())
    , mHasHiResTrack(session.hasHighResolutionTrack())
    , mHasLowResTrack(session.hasLowResolutionTrack())
{
}

MegaChatSessionPrivate::MegaChatSessionPrivate(const MegaChatSessionPrivate &session)
    : mState(session.getStatus())
    , mPeerId(session.getPeerid())
    , mClientId(session.getClientid())
    , mAvFlags(session.getAvFlags())
    , mChanged(session.getChanges())
    , mHasRequestSpeak(session.hasRequestSpeak())
    , mAudioDetected(session.isAudioDetected())
    , mHasHiResTrack(session.mHasHiResTrack)
    , mHasLowResTrack(session.mHasLowResTrack)
{
}

MegaChatSessionPrivate::~MegaChatSessionPrivate()
{
}

MegaChatSession *MegaChatSessionPrivate::copy()
{
    return new MegaChatSessionPrivate(*this);
}

int MegaChatSessionPrivate::getStatus() const
{
    return mState;
}

MegaChatHandle MegaChatSessionPrivate::getPeerid() const
{
    return mPeerId;
}

MegaChatHandle MegaChatSessionPrivate::getClientid() const
{
    return mClientId;
}

bool MegaChatSessionPrivate::hasAudio() const
{
    return mAvFlags.audio();
}

bool MegaChatSessionPrivate::hasVideo() const
{
    return mAvFlags.video();
}

bool MegaChatSessionPrivate::isHiResVideo() const
{
    return mAvFlags.videoHiRes();
}

bool MegaChatSessionPrivate::isLowResVideo() const
{
    return mAvFlags.videoLowRes();
}

bool MegaChatSessionPrivate::isOnHold() const
{
    return mAvFlags.isOnHold();
}

int MegaChatSessionPrivate::getChanges() const
{
    return mChanged;
}

bool MegaChatSessionPrivate::hasChanged(int changeType) const
{
    return (mChanged & changeType);
}

karere::AvFlags MegaChatSessionPrivate::getAvFlags() const
{
    return mAvFlags;
}

bool MegaChatSessionPrivate::isAudioDetected() const
{
    return mAudioDetected;
}

bool MegaChatSessionPrivate::hasRequestSpeak() const
{
    return mHasRequestSpeak;
}

bool MegaChatSessionPrivate::canRecvVideoHiRes() const
{
    return mHasHiResTrack;
}

bool MegaChatSessionPrivate::canRecvVideoLowRes() const
{
    return mHasLowResTrack;
}

void MegaChatSessionPrivate::setState(uint8_t state)
{
    mState = state;
    mChanged |= MegaChatSession::CHANGE_TYPE_STATUS;
}

void MegaChatSessionPrivate::setAudioDetected(bool audioDetected)
{
    mAudioDetected = audioDetected;
    mChanged |= CHANGE_TYPE_AUDIO_LEVEL;
}

void MegaChatSessionPrivate::setOnHold(bool onHold)
{
    mAvFlags.setOnHold(onHold);
    mChanged |= CHANGE_TYPE_SESSION_ON_HOLD;
}

void MegaChatSessionPrivate::setChange(int change)
{
    mChanged |= change;
}

void MegaChatSessionPrivate::removeChanges()
{
    mChanged = MegaChatSession::CHANGE_TYPE_NO_CHANGES;
}

MegaChatCallPrivate::MegaChatCallPrivate(const rtcModule::ICall &call)
{
    mChatid = call.getChatid();
    mCallId = call.getCallid();
    mStatus = call.getState();
    mCallerId = call.getCallerid();
    mIsCaller = call.isOutgoing();
    mIgnored = call.isIgnored();
    mIsSpeakAllow = call.isSpeakAllow();
    mLocalAVFlags = call.getLocalAvFlags();
    mInitialTs = call.getInitialTimeStamp() - call.getInitialOffset();
    mFinalTs = call.getFinalTimeStamp();
    mAudioDetected = call.isAudioDetected();
    mNetworkQuality = call.getNetworkQuality();
    mHasRequestSpeak = call.hasRequestSpeak();
    mTermCode = convertTermCode(call.getTermCode());

    for (auto participant: call.getParticipants())
    {
        mParticipants.push_back(participant);
    }
    mRinging = call.isRinging();

    std::vector<Cid_t> sessionCids = call.getSessionsCids();
    for (Cid_t cid : sessionCids)
    {
        mSessions[cid] = ::mega::make_unique<MegaChatSessionPrivate>(*call.getIsession(cid));
    }
}

MegaChatCallPrivate::MegaChatCallPrivate(const MegaChatCallPrivate &call)
{
    mStatus = call.getStatus();
    mChatid = call.getChatid();
    mCallId = call.getCallId();
    mIsCaller = call.isOutgoing();
    mLocalAVFlags = call.mLocalAVFlags;
    mChanged = call.mChanged;
    mInitialTs = call.mInitialTs;
    mFinalTs = call.mFinalTs;
    mTermCode = call.mTermCode;
    mRinging = call.mRinging;
    mIgnored = call.mIgnored;
    mPeerId = call.mPeerId;
    mCallCompositionChange = call.mCallCompositionChange;
    mCallerId = call.mCallerId;
    mIsSpeakAllow = call.isSpeakAllow();
    mAudioDetected = call.isAudioDetected();
    mNetworkQuality = call.getNetworkQuality();
    mHasRequestSpeak = call.hasRequestSpeak();

    for (auto it = call.mSessions.begin(); it != call.mSessions.end(); it++)
    {
        mSessions[it->first] = std::unique_ptr<MegaChatSession>(it->second->copy());
    }

    mParticipants = call.mParticipants;
}

MegaChatCallPrivate::~MegaChatCallPrivate()
{
    mSessions.clear();
}

MegaChatCall *MegaChatCallPrivate::copy()
{
    return new MegaChatCallPrivate(*this);
}

int MegaChatCallPrivate::getStatus() const
{
    return mStatus;
}

MegaChatHandle MegaChatCallPrivate::getChatid() const
{
    return mChatid;
}

MegaChatHandle MegaChatCallPrivate::getCallId() const
{
    return mCallId;
}

bool MegaChatCallPrivate::hasLocalAudio() const
{
    return mLocalAVFlags.audio();
}

bool MegaChatCallPrivate::hasLocalVideo() const
{
    return mLocalAVFlags.videoCam();
}

int MegaChatCallPrivate::getChanges() const
{
    return mChanged;
}

bool MegaChatCallPrivate::isAudioDetected() const
{
    return mAudioDetected;
}

bool MegaChatCallPrivate::hasChanged(int changeType) const
{
    return (mChanged & changeType);
}

int64_t MegaChatCallPrivate::getDuration() const
{
    int64_t duration = 0;

    if (mInitialTs > 0)
    {
        if (mFinalTs > 0)
        {
            duration = mFinalTs - mInitialTs;
        }
        else
        {
            duration = time(NULL) - mInitialTs;
        }
    }

    return duration;
}

int64_t MegaChatCallPrivate::getInitialTimeStamp() const
{
    return mInitialTs;
}

int64_t MegaChatCallPrivate::getFinalTimeStamp() const
{
    return mFinalTs;
}

int MegaChatCallPrivate::getTermCode() const
{
    return mTermCode;
}

bool MegaChatCallPrivate::isRinging() const
{
    return mRinging;
}

MegaHandleList *MegaChatCallPrivate::getSessionsClientid() const
{
    MegaHandleListPrivate *sessionList = new MegaHandleListPrivate();

    for (auto it = mSessions.begin(); it != mSessions.end(); it++)
    {
        sessionList->addMegaHandle(it->first);
    }

    return sessionList;
}

MegaChatHandle MegaChatCallPrivate::getPeeridCallCompositionChange() const
{
    return mPeerId;
}

int MegaChatCallPrivate::getCallCompositionChange() const
{
    return mCallCompositionChange;
}

MegaChatSession *MegaChatCallPrivate::getMegaChatSession(MegaChatHandle clientId)
{
    auto it = mSessions.find(clientId);
    if (it != mSessions.end())
    {
        return it->second.get();
    }

    return NULL;
}

int MegaChatCallPrivate::getNumParticipants() const
{
    return static_cast<int>(mParticipants.size());
}

MegaHandleList *MegaChatCallPrivate::getPeeridParticipants() const
{
    MegaHandleListPrivate *participantsList = new MegaHandleListPrivate();

    for (const MegaChatHandle& participant : mParticipants)
    {
        participantsList->addMegaHandle(participant);
    }

    return participantsList;
}

bool MegaChatCallPrivate::isIgnored() const
{
    return mIgnored;
}

bool MegaChatCallPrivate::isIncoming() const
{
    return !mIsCaller;
}

bool MegaChatCallPrivate::isOutgoing() const
{
    return mIsCaller;
}

MegaChatHandle MegaChatCallPrivate::getCaller() const
{
    return mCallerId;
}

bool MegaChatCallPrivate::isOnHold() const
{
    return mLocalAVFlags.isOnHold();
}

bool MegaChatCallPrivate::isSpeakAllow() const
{
    return mIsSpeakAllow;
}

int MegaChatCallPrivate::getNetworkQuality() const
{
    return mNetworkQuality;
}

bool MegaChatCallPrivate::hasRequestSpeak() const
{
    return mHasRequestSpeak;
}

void MegaChatCallPrivate::setStatus(int status)
{
    mStatus = status;
    mChanged |= MegaChatCall::CHANGE_TYPE_STATUS;

    if (status == MegaChatCall::CALL_STATUS_DESTROYED)
    {
        API_LOG_INFO("Call Destroyed. ChatId: %s, callid: %s, duration: %d (s)",
                     karere::Id(getChatid()).toString().c_str(),
                     karere::Id(getCallId()).toString().c_str(), getDuration());
    }
}

void MegaChatCallPrivate::setLocalAudioVideoFlags(AvFlags localAVFlags)
{
    if (mLocalAVFlags == localAVFlags)
    {
        return;
    }

    mLocalAVFlags = localAVFlags;
    mChanged |= MegaChatCall::CHANGE_TYPE_LOCAL_AVFLAGS;
}

void MegaChatCallPrivate::removeChanges()
{
    mChanged = MegaChatCall::CHANGE_TYPE_NO_CHANGES;
    mCallCompositionChange = NO_COMPOSITION_CHANGE;
}

void MegaChatCallPrivate::setChange(int changed)
{
    mChanged = changed;
}

int MegaChatCallPrivate::convertCallState(rtcModule::CallState newState)
{
    // todo: implement additional operations associated to the state change
    int state = 0;
    switch(newState)
    {
        case rtcModule::CallState::kStateInitial:
            state = MegaChatCall::CALL_STATUS_INITIAL;
            break;
        case rtcModule::CallState::kStateClientNoParticipating:
            state = MegaChatCall::CALL_STATUS_USER_NO_PRESENT;
            break;
        case rtcModule::CallState::kStateConnecting:
            state = MegaChatCall::CALL_STATUS_CONNECTING;
            break;
        case rtcModule::CallState::kStateJoining:
            state = MegaChatCall::CALL_STATUS_JOINING;
            break;
        case rtcModule::CallState::kStateInProgress:
            state = MegaChatCall::CALL_STATUS_IN_PROGRESS;
            break;
        case rtcModule::CallState::kStateTerminatingUserParticipation:
            state = MegaChatCall::CALL_STATUS_TERMINATING_USER_PARTICIPATION;
            break;
        case rtcModule::CallState::kStateDestroyed:
            state = MegaChatCall::CALL_STATUS_DESTROYED;
            break;
    }
    return state;
}

int MegaChatCallPrivate::convertTermCode(rtcModule::TermCode termCode)
{
    switch (termCode)
    {
        case rtcModule::TermCode::kErrSdp:
        case rtcModule::TermCode::kErrNoCall:
        case rtcModule::TermCode::kRtcDisconn:
        case rtcModule::TermCode::kSigDisconn:
        case rtcModule::TermCode::kErrSignaling:
        case rtcModule::TermCode::kSvrShuttingDown:
        case rtcModule::TermCode::kUnKnownTermCode:
            return TERM_CODE_ERROR;

        case rtcModule::TermCode::kUserHangup:
            return TERM_CODE_HANGUP;

       case rtcModule::TermCode::kInvalidTermCode:
            return TERM_CODE_INVALID;
    }

    return TERM_CODE_INVALID;
}

void MegaChatCallPrivate::setIsRinging(bool ringing)
{
    mRinging = ringing;
    mChanged |= MegaChatCall::CHANGE_TYPE_RINGING_STATUS;
}

MegaChatSessionPrivate *MegaChatCallPrivate::addSession(rtcModule::ISession &/*sess*/)
{
    return nullptr;
}

int MegaChatCallPrivate::availableAudioSlots()
{
    return 0;
}

int MegaChatCallPrivate::availableVideoSlots()
{
    return 0;
}

void MegaChatCallPrivate::setPeerid(Id peerid, bool added)
{
    mPeerId = peerid;
    mChanged = MegaChatCall::CHANGE_TYPE_CALL_COMPOSITION;
    if (added)
    {
        mCallCompositionChange = MegaChatCall::PEER_ADDED;
    }
    else
    {
        mCallCompositionChange = MegaChatCall::PEER_REMOVED;
    }
}

bool MegaChatCallPrivate::isParticipating(Id userid)
{
    return std::find(mParticipants.begin(), mParticipants.end(), userid) != mParticipants.end();
}

void MegaChatCallPrivate::setId(Id callid)
{
    mCallId = callid;
}

void MegaChatCallPrivate::setCaller(Id caller)
{
    mCallerId = caller;
}

void MegaChatCallPrivate::setOnHold(bool onHold)
{
    mLocalAVFlags.setOnHold(onHold);
    mChanged |= MegaChatCall::CHANGE_TYPE_CALL_ON_HOLD;
}

void MegaChatCallPrivate::setAudioDetected(bool audioDetected)
{
    mAudioDetected = audioDetected;
    mChanged |= MegaChatCall::CHANGE_TYPE_AUDIO_LEVEL;
}

MegaChatVideoReceiver::MegaChatVideoReceiver(MegaChatApiImpl *chatApi, karere::Id chatid, rtcModule::VideoResolution videoResolution, uint32_t clientId)
{
    mChatApi = chatApi;
    mChatid = chatid;
    mVideoResolution = videoResolution;
    mClientId = clientId;
}

MegaChatVideoReceiver::~MegaChatVideoReceiver()
{
}

void* MegaChatVideoReceiver::getImageBuffer(unsigned short width, unsigned short height, void*& userData)
{
    MegaChatVideoFrame *frame = new MegaChatVideoFrame;
    frame->width = width;
    frame->height = height;
    frame->buffer = new ::mega::byte[width * height * 4];  // in format ARGB: 4 bytes per pixel
    userData = frame;
    return frame->buffer;
}

void MegaChatVideoReceiver::frameComplete(void *userData)
{
    mChatApi->videoMutex.lock();
    MegaChatVideoFrame *frame = (MegaChatVideoFrame *)userData;
    mChatApi->fireOnChatVideoData(mChatid, mClientId, frame->width, frame->height, (char *)frame->buffer, mClientId ? mVideoResolution : rtcModule::VideoResolution::kHiRes);
    mChatApi->videoMutex.unlock();
    delete [] frame->buffer;
    delete frame;
}

void MegaChatVideoReceiver::onVideoAttach()
{
}

void MegaChatVideoReceiver::onVideoDetach()
{
}

void MegaChatVideoReceiver::clearViewport()
{
}

void MegaChatVideoReceiver::released()
{
}

#endif

MegaChatRoomHandler::MegaChatRoomHandler(MegaChatApiImpl *chatApiImpl, MegaChatApi *chatApi, MegaApi *megaApi, MegaChatHandle chatid)
{
    mChatApiImpl = chatApiImpl;
    mChatApi = chatApi;
    mChatid = chatid;
    mMegaApi = megaApi;

    mRoom = NULL;
    mChat = NULL;
}

void MegaChatRoomHandler::addChatRoomListener(MegaChatRoomListener *listener)
{
    roomListeners.insert(listener);
}

void MegaChatRoomHandler::removeChatRoomListener(MegaChatRoomListener *listener)
{
    roomListeners.erase(listener);
}

void MegaChatRoomHandler::fireOnChatRoomUpdate(MegaChatRoom *chat)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onChatRoomUpdate(mChatApi, chat);
    }

    delete chat;
}

void MegaChatRoomHandler::fireOnMessageLoaded(MegaChatMessage *msg)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end(); it++)
    {
        (*it)->onMessageLoaded(mChatApi, msg);
    }

    delete msg;
}

void MegaChatRoomHandler::fireOnHistoryTruncatedByRetentionTime(MegaChatMessage *msg)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onHistoryTruncatedByRetentionTime(mChatApi, msg);
    }

    delete msg;
}

void MegaChatRoomHandler::fireOnMessageReceived(MegaChatMessage *msg)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onMessageReceived(mChatApi, msg);
    }

    delete msg;
}

void MegaChatRoomHandler::fireOnReactionUpdate(MegaChatHandle msgid, const char *reaction, int count)
{
    for (set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end(); it++)
    {
        (*it)->onReactionUpdate(mChatApi, msgid, reaction, count);
    }
}

void MegaChatRoomHandler::fireOnMessageUpdate(MegaChatMessage *msg)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onMessageUpdate(mChatApi, msg);
    }

    delete msg;
}

void MegaChatRoomHandler::fireOnHistoryReloaded(MegaChatRoom *chat)
{
    for(set<MegaChatRoomListener *>::iterator it = roomListeners.begin(); it != roomListeners.end() ; it++)
    {
        (*it)->onHistoryReloaded(mChatApi, chat);
    }

    delete chat;
}

void MegaChatRoomHandler::onUserTyping(karere::Id user)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setUserTyping(user.val);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onReactionUpdate(karere::Id msgid, const char *reaction, int count)
{
    fireOnReactionUpdate(msgid, reaction, count);
}

void MegaChatRoomHandler::onUserStopTyping(karere::Id user)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setUserStopTyping(user.val);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onLastTextMessageUpdated(const chatd::LastTextMsg& msg)
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onLastTextMessageUpdated(msg);
    }
}

void MegaChatRoomHandler::onLastMessageTsUpdated(uint32_t ts)
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onLastMessageTsUpdated(ts);
    }
}

void MegaChatRoomHandler::onHistoryReloaded()
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    fireOnHistoryReloaded(chat);
}

bool MegaChatRoomHandler::isRevoked(MegaChatHandle h)
{
    auto it = attachmentsAccess.find(h);
    if (it != attachmentsAccess.end())
    {
        return !it->second;
    }

    return false;
}

void MegaChatRoomHandler::handleHistoryMessage(MegaChatMessage *message)
{
    if (message->getType() == MegaChatMessage::TYPE_NODE_ATTACHMENT)
    {
        MegaNodeList *nodeList = message->getMegaNodeList();
        if (nodeList)
        {
            for (int i = 0; i < nodeList->size(); i++)
            {
                MegaChatHandle h = nodeList->get(i)->getHandle();
                auto itAccess = attachmentsAccess.find(h);
                if (itAccess == attachmentsAccess.end())
                {
                    attachmentsAccess[h] = true;
                }
                attachmentsIds[h].insert(message->getMsgId());
            }
        }
    }
    else if (message->getType() == MegaChatMessage::TYPE_REVOKE_NODE_ATTACHMENT)
    {
        MegaChatHandle h = message->getHandleOfAction();
        auto itAccess = attachmentsAccess.find(h);
        if (itAccess == attachmentsAccess.end())
        {
            attachmentsAccess[h] = false;
        }
    }
}

std::set<MegaChatHandle> *MegaChatRoomHandler::handleNewMessage(MegaChatMessage *message)
{
    set <MegaChatHandle> *msgToUpdate = NULL;

    // new messages overwrite any current access to nodes
    if (message->getType() == MegaChatMessage::TYPE_NODE_ATTACHMENT)
    {
        MegaNodeList *nodeList = message->getMegaNodeList();
        if (nodeList)
        {
            for (int i = 0; i < nodeList->size(); i++)
            {
                MegaChatHandle h = nodeList->get(i)->getHandle();
                auto itAccess = attachmentsAccess.find(h);
                if (itAccess != attachmentsAccess.end() && !itAccess->second)
                {
                    // access changed from revoked to granted --> update attachment messages
                    if (!msgToUpdate)
                    {
                        msgToUpdate = new set <MegaChatHandle>;
                    }
                    msgToUpdate->insert(attachmentsIds[h].begin(), attachmentsIds[h].end());
                }
                attachmentsAccess[h] = true;
                attachmentsIds[h].insert(message->getMsgId());
            }
        }
    }
    else if (message->getType() == MegaChatMessage::TYPE_REVOKE_NODE_ATTACHMENT)
    {
        MegaChatHandle h = message->getHandleOfAction();
        auto itAccess = attachmentsAccess.find(h);
        if (itAccess != attachmentsAccess.end() && itAccess->second)
        {
            // access changed from granted to revoked --> update attachment messages
            if (!msgToUpdate)
            {
                msgToUpdate = new set <MegaChatHandle>;
            }
            msgToUpdate->insert(attachmentsIds[h].begin(), attachmentsIds[h].end());
        }
        attachmentsAccess[h] = false;
    }

    return msgToUpdate;
}

void MegaChatRoomHandler::onMemberNameChanged(uint64_t userid, const std::string &/*newName*/)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setMembersUpdated(userid);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onChatArchived(bool archived)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setArchived(archived);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onTitleChanged(const string &title)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setTitle(title);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onChatModeChanged(bool mode)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setChatMode(mode);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onUnreadCountChanged()
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->changeUnreadCount();

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onPreviewersCountUpdate(uint32_t numPrev)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setNumPreviewers(numPrev);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::init(Chat &chat, DbInterface *&)
{
    mChat = &chat;
    mRoom = mChatApiImpl->findChatRoom(mChatid);

    attachmentsAccess.clear();
    attachmentsIds.clear();
    mChat->resetListenerState();
}

void MegaChatRoomHandler::onDestroy()
{
    mChat = NULL;
    mRoom = NULL;
    attachmentsAccess.clear();
    attachmentsIds.clear();
}

void MegaChatRoomHandler::onRecvNewMessage(Idx idx, Message &msg, Message::Status status)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    set <MegaChatHandle> *msgToUpdate = handleNewMessage(message);

    fireOnMessageReceived(message);

    if (msgToUpdate)
    {
        for (auto itMsgId = msgToUpdate->begin(); itMsgId != msgToUpdate->end(); itMsgId++)
        {
            MegaChatMessagePrivate *msg = (MegaChatMessagePrivate *)mChatApiImpl->getMessage(mChatid, *itMsgId);
            if (msg)
            {
                msg->setAccess();
                fireOnMessageUpdate(msg);
            }
        }
        delete msgToUpdate;
    }

    // check if notification is required
    if (mRoom && mMegaApi->isChatNotifiable(mChatid)
            && ((msg.type == chatd::Message::kMsgTruncate)   // truncate received from a peer or from myself in another client
                || (msg.userid != mChatApi->getMyUserHandle() && status == chatd::Message::kNotSeen)))  // new (unseen) message received from a peer
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onRecvNewMessage(idx, msg, status);
    }
}

void MegaChatRoomHandler::onRecvHistoryMessage(Idx idx, Message &msg, Message::Status status, bool /*isLocal*/)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    handleHistoryMessage(message);

    fireOnMessageLoaded(message);
}

void MegaChatRoomHandler::onHistoryDone(chatd::HistSource /*source*/)
{
    fireOnMessageLoaded(NULL);
}

void MegaChatRoomHandler::onHistoryTruncatedByRetentionTime(const Message &msg, const Idx &idx, const Message::Status &status)
{
   MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
   fireOnHistoryTruncatedByRetentionTime(message);
}

void MegaChatRoomHandler::onUnsentMsgLoaded(chatd::Message &msg)
{
    Message::Status status = (Message::Status) MegaChatMessage::STATUS_SENDING;
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, MEGACHAT_INVALID_INDEX);
    fireOnMessageLoaded(message);
}

void MegaChatRoomHandler::onUnsentEditLoaded(chatd::Message &msg, bool oriMsgIsSending)
{
    Idx index = MEGACHAT_INVALID_INDEX;
    if (!oriMsgIsSending)   // original message was already sent
    {
        index = mChat->msgIndexFromId(msg.id());
    }
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, Message::kSending, index);
    message->setContentChanged();
    fireOnMessageLoaded(message);
}

void MegaChatRoomHandler::onMessageConfirmed(Id msgxid, const Message &msg, Idx idx, bool tsUpdated)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, Message::kServerReceived, idx);
    message->setStatus(MegaChatMessage::STATUS_SERVER_RECEIVED);
    message->setTempId(msgxid);     // to allow the app to find the "temporal" message
    if (tsUpdated)
    {
        message->setTsUpdated();
    }

    std::set <MegaChatHandle> *msgToUpdate = handleNewMessage(message);

    fireOnMessageUpdate(message);

    if (msgToUpdate)
    {
        for (auto itMsgId = msgToUpdate->begin(); itMsgId != msgToUpdate->end(); itMsgId++)
        {
            MegaChatMessagePrivate *msgUpdated = (MegaChatMessagePrivate *)mChatApiImpl->getMessage(mChatid, *itMsgId);
            if (msgUpdated)
            {
                msgUpdated->setAccess();
                fireOnMessageUpdate(msgUpdated);
            }
        }
        delete msgToUpdate;
    }
}

void MegaChatRoomHandler::onMessageRejected(const Message &msg, uint8_t reason)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, Message::kServerRejected, MEGACHAT_INVALID_INDEX);
    message->setStatus(MegaChatMessage::STATUS_SERVER_REJECTED);
    message->setCode(reason);
    fireOnMessageUpdate(message);
}

void MegaChatRoomHandler::onMessageStatusChange(Idx idx, Message::Status status, const Message &msg)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    message->setStatus(status);
    fireOnMessageUpdate(message);

    if (mMegaApi->isChatNotifiable(mChatid)
            && msg.userid != mChatApi->getMyUserHandle()
            && status == chatd::Message::kSeen  // received message from a peer changed to seen
            && !msg.isEncrypted())  // messages can be "seen" while being decrypted
    {
        MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
        mChatApiImpl->fireOnChatNotification(mChatid, message);
    }
}

void MegaChatRoomHandler::onMessageEdited(const Message &msg, chatd::Idx idx)
{
    Message::Status status = mChat->getMsgStatus(msg, idx);
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
    message->setContentChanged();
    fireOnMessageUpdate(message);

    //TODO: check a truncate always comes as an edit, even if no history exist at all (new chat)
    // and, if so, remove the block from `onRecvNewMessage()`
    if (mMegaApi->isChatNotifiable(mChatid) &&
            ((msg.type == chatd::Message::kMsgTruncate) // truncate received from a peer or from myself in another client
             || (msg.userid != mChatApi->getMyUserHandle() && status == chatd::Message::kNotSeen)))    // received message from a peer, still unseen, was edited / deleted
    {
        MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, status, idx);
        mChatApiImpl->fireOnChatNotification(mChatid, message);
    }
}

void MegaChatRoomHandler::onEditRejected(const Message &msg, ManualSendReason reason)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(msg, Message::kSendingManual, MEGACHAT_INVALID_INDEX);
    if (reason == ManualSendReason::kManualSendEditNoChange)
    {
        API_LOG_WARNING("Edit message rejected because of same content");
        message->setStatus(mChat->getMsgStatus(msg, msg.id()));
    }
    else
    {
        API_LOG_WARNING("Edit message rejected, reason: %d", reason);
        message->setCode(reason);
    }
    fireOnMessageUpdate(message);
}

void MegaChatRoomHandler::onOnlineStateChange(ChatState state)
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onOnlineStateChange(state);
    }
}

void MegaChatRoomHandler::onUserJoin(Id userid, Priv privilege)
{
    if (mRoom)
    {
        const GroupChatRoom* groupChatRoom = dynamic_cast<const GroupChatRoom *>(mRoom);
        if (groupChatRoom)
        {
            auto it = groupChatRoom->peers().find(userid);
            if (it != groupChatRoom->peers().end() && it->second->priv() == privilege)
            {
                return;
            }
        }

        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onUserJoin(userid, privilege);

        // avoid to notify if own user doesn't participate or isn't online and it's a public chat (for large chat-links, for performance)
        if (mRoom->publicChat() && (mRoom->chat().onlineState() != kChatStateOnline || mRoom->chat().getOwnprivilege() == chatd::Priv::PRIV_NOTPRESENT))
        {
            return;
        }

        MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
        if (userid.val == mChatApiImpl->getMyUserHandle())
        {
            chatroom->setOwnPriv(privilege);
        }
        else
        {
            chatroom->setMembersUpdated(userid);
        }
        fireOnChatRoomUpdate(chatroom);
    }
}

void MegaChatRoomHandler::onUserLeave(Id userid)
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onUserLeave(userid);

        if (mRoom->publicChat() && mRoom->chat().getOwnprivilege() == chatd::Priv::PRIV_NOTPRESENT)
        {
            return;
        }

        MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
        chatroom->setMembersUpdated(userid);
        fireOnChatRoomUpdate(chatroom);
    }
}

void MegaChatRoomHandler::onRejoinedChat()
{
    if (mRoom)
    {
        MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
        fireOnChatRoomUpdate(chatroom);
    }
}

void MegaChatRoomHandler::onExcludedFromChat()
{
    if (mRoom)
    {
        MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
        chatroom->setClosed();
        fireOnChatRoomUpdate(chatroom);
    }
}

void MegaChatRoomHandler::onUnreadChanged()
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onUnreadChanged();

        if (mChat)
        {
            MegaChatRoomPrivate *chatroom = new MegaChatRoomPrivate(*mRoom);
            chatroom->changeUnreadCount();
            fireOnChatRoomUpdate(chatroom);
        }
    }
}

void MegaChatRoomHandler::onRetentionTimeUpdated(unsigned int period)
{
    MegaChatRoomPrivate *chat = (MegaChatRoomPrivate *) mChatApiImpl->getChatRoom(mChatid);
    chat->setRetentionTime(period);

    fireOnChatRoomUpdate(chat);
}

void MegaChatRoomHandler::onPreviewersUpdate()
{
    if (mRoom)
    {
        // forward the event to the chatroom, so chatlist items also receive the notification
        mRoom->onPreviewersUpdate();
        onPreviewersCountUpdate(mChat->getNumPreviewers());
    }
}

void MegaChatRoomHandler::onManualSendRequired(chatd::Message *msg, uint64_t id, chatd::ManualSendReason reason)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(*msg, Message::kSendingManual, MEGACHAT_INVALID_INDEX);
    delete msg; // we take ownership of the Message

    message->setStatus(MegaChatMessage::STATUS_SENDING_MANUAL);
    message->setRowId(id); // identifier for the manual-send queue, for removal from queue
    message->setCode(reason);
    fireOnMessageLoaded(message);
}


MegaChatErrorPrivate::MegaChatErrorPrivate(const string &msg, int code, int type)
    : ::promise::Error(msg, code, type)
{
    setHandled();
}

MegaChatErrorPrivate::MegaChatErrorPrivate(int code, int type)
    : ::promise::Error(MegaChatErrorPrivate::getGenericErrorString(code), code, type)
{
    setHandled();
}

const char* MegaChatErrorPrivate::getGenericErrorString(int errorCode)
{
    switch(errorCode)
    {
    case ERROR_OK:
        return "No error";
    case ERROR_ARGS:
        return "Invalid argument";
    case ERROR_TOOMANY:
        return "Too many uses for this resource";
    case ERROR_ACCESS:
        return "Access denied";
    case ERROR_NOENT:
        return "Resource does not exist";
    case ERROR_EXIST:
        return "Resource already exists";
    case ERROR_UNKNOWN:
    default:
        return "Unknown error";
    }
}


MegaChatErrorPrivate::MegaChatErrorPrivate(const MegaChatErrorPrivate *error)
    : ::promise::Error(error->getErrorString(), error->getErrorCode(), error->getErrorType())
{
    setHandled();
}

int MegaChatErrorPrivate::getErrorCode() const
{
    return code();
}

int MegaChatErrorPrivate::getErrorType() const
{
    return type();
}

const char *MegaChatErrorPrivate::getErrorString() const
{
    return what();
}

const char *MegaChatErrorPrivate::toString() const
{
    char *errorString = new char[msg().size()+1];
    strcpy(errorString, what());
    return errorString;
}

MegaChatError *MegaChatErrorPrivate::copy()
{
    return new MegaChatErrorPrivate(this);
}


MegaChatRoomListPrivate::MegaChatRoomListPrivate()
{

}

MegaChatRoomListPrivate::MegaChatRoomListPrivate(const MegaChatRoomListPrivate *list)
{
    MegaChatRoomPrivate *chat;

    for (unsigned int i = 0; i < list->size(); i++)
    {
        chat = new MegaChatRoomPrivate(list->get(i));
        mList.push_back(chat);
    }
}

MegaChatRoomList *MegaChatRoomListPrivate::copy() const
{
    return new MegaChatRoomListPrivate(this);
}

const MegaChatRoom *MegaChatRoomListPrivate::get(unsigned int i) const
{
    if (i >= size())
    {
        return NULL;
    }
    else
    {
        return mList.at(i);
    }
}

unsigned int MegaChatRoomListPrivate::size() const
{
    return mList.size();
}

void MegaChatRoomListPrivate::addChatRoom(MegaChatRoom *chat)
{
    mList.push_back(chat);
}


MegaChatRoomPrivate::MegaChatRoomPrivate(const MegaChatRoom *chat)
{
    mChatid = chat->getChatId();
    priv = (privilege_t) chat->getOwnPrivilege();
    for (unsigned int i = 0; i < chat->getPeerCount(); i++)
    {
        MegaChatHandle uh = chat->getPeerHandle(i);
        mPeers.push_back(userpriv_pair(uh, (privilege_t) chat->getPeerPrivilege(i)));
        if (chat->getPeerFirstname(i) && chat->getPeerLastname(i) && chat->getPeerEmail(i))
        {
            peerFirstnames.push_back(chat->getPeerFirstname(i));
            peerLastnames.push_back(chat->getPeerLastname(i));
            peerEmails.push_back(chat->getPeerEmail(i));
        }
        else
        {
            assert(!chat->getPeerEmail(i) && !chat->getPeerLastname(i) && !chat->getPeerEmail(i));
        }
    }
    group = chat->isGroup();
    mPublicChat = chat->isPublic();
    mAuthToken = chat->getAuthorizationToken() ? Id(chat->getAuthorizationToken()) : Id::inval();
    mTitle = chat->getTitle();
    mHasCustomTitle = chat->hasCustomTitle();
    unreadCount = chat->getUnreadCount();
    active = chat->isActive();
    mArchived = chat->isArchived();
    mChanged = chat->getChanges();
    mUh = chat->getUserTyping();
    mNumPreviewers = chat->getNumPreviewers();
    mRetentionTime = chat->getRetentionTime();
    mCreationTs = chat->getCreationTs();
    mMeeting = chat->isMeeting();
}

MegaChatRoomPrivate::MegaChatRoomPrivate(const ChatRoom &chat)
{
    mChanged = 0;
    mChatid = chat.chatid();
    priv = (privilege_t) chat.ownPriv();
    group = chat.isGroup();
    mPublicChat = chat.publicChat();
    mAuthToken = Id(chat.getPublicHandle());
    assert(!chat.previewMode() || (chat.previewMode() && mAuthToken.isValid()));
    mTitle = chat.titleString();
    mHasCustomTitle = chat.isGroup() ? ((GroupChatRoom*)&chat)->hasTitle() : false;
    unreadCount = chat.chat().unreadMsgCount();
    active = chat.isActive();
    mArchived = chat.isArchived();
    mUh = MEGACHAT_INVALID_HANDLE;
    mNumPreviewers = chat.chat().getNumPreviewers();
    mRetentionTime = chat.getRetentionTime();
    mCreationTs = chat.getCreationTs();
    mMeeting = chat.isMeeting();

    if (group)
    {
        GroupChatRoom &groupchat = (GroupChatRoom&) chat;
        const GroupChatRoom::MemberMap& peers = groupchat.peers();

        GroupChatRoom::MemberMap::const_iterator it;
        for (it = peers.begin(); it != peers.end(); it++)
        {
            mPeers.push_back(userpriv_pair(it->first, (privilege_t) it->second->priv()));

            if (!chat.publicChat() || chat.numMembers() < PRELOAD_CHATLINK_PARTICIPANTS)
            {
                const char *buffer = MegaChatRoomPrivate::firstnameFromBuffer(it->second->name());
                peerFirstnames.push_back(buffer ? buffer : "");
                delete [] buffer;

                buffer = MegaChatRoomPrivate::lastnameFromBuffer(it->second->name());
                peerLastnames.push_back(buffer ? buffer : "");
                delete [] buffer;

                peerEmails.push_back(it->second->email());
            }
        }
    }
    else
    {
        PeerChatRoom &peerchat = (PeerChatRoom&) chat;
        privilege_t priv = (privilege_t) peerchat.peerPrivilege();
        handle uh = peerchat.peer();
        mPeers.push_back(userpriv_pair(uh, priv));

        Contact *contact = peerchat.contact();
        if (contact)
        {
            string name = contact->getContactName(true);

            const char *buffer = MegaChatRoomPrivate::firstnameFromBuffer(name);
            peerFirstnames.push_back(buffer ? buffer : "");
            delete [] buffer;

            buffer = MegaChatRoomPrivate::lastnameFromBuffer(name);
            peerLastnames.push_back(buffer ? buffer : "");
            delete [] buffer;
        }
        else    // we don't have firstname and lastname individually
        {
            peerFirstnames.push_back(mTitle);
            peerLastnames.push_back("");
        }


        peerEmails.push_back(peerchat.email());
    }
}

MegaChatRoom *MegaChatRoomPrivate::copy() const
{
    return new MegaChatRoomPrivate(this);
}

MegaChatHandle MegaChatRoomPrivate::getChatId() const
{
    return mChatid;
}

int MegaChatRoomPrivate::getOwnPrivilege() const
{
    return priv;
}

unsigned int MegaChatRoomPrivate::getNumPreviewers() const
{
   return mNumPreviewers;
}

int MegaChatRoomPrivate::getPeerPrivilegeByHandle(MegaChatHandle userhandle) const
{
    for (unsigned int i = 0; i < mPeers.size(); i++)
    {
        if (mPeers.at(i).first == userhandle)
        {
            return mPeers.at(i).second;
        }
    }

    return PRIV_UNKNOWN;
}

const char *MegaChatRoomPrivate::getPeerFirstnameByHandle(MegaChatHandle userhandle) const
{
    for (unsigned int i = 0; i < peerFirstnames.size(); i++)
    {
        assert(i < mPeers.size());
        if (mPeers.at(i).first == userhandle)
        {
            return (!peerFirstnames.at(i).empty()) ? peerFirstnames.at(i).c_str() : nullptr;
        }
    }

    return nullptr;
}

const char *MegaChatRoomPrivate::getPeerLastnameByHandle(MegaChatHandle userhandle) const
{
    for (unsigned int i = 0; i < peerLastnames.size(); i++)
    {
        assert(i < mPeers.size());
        if (mPeers.at(i).first == userhandle)
        {
            return (!peerLastnames.at(i).empty()) ? peerLastnames.at(i).c_str() : nullptr;
        }
    }

    return nullptr;
}

const char *MegaChatRoomPrivate::getPeerFullnameByHandle(MegaChatHandle userhandle) const
{
    for (unsigned int i = 0; i < peerFirstnames.size(); i++)
    {
        assert(i < mPeers.size());
        if (mPeers.at(i).first == userhandle)
        {
            string ret = peerFirstnames.at(i);
            if (!peerFirstnames.at(i).empty() && !peerLastnames.at(i).empty())
            {
                ret.append(" ");
            }
            ret.append(peerLastnames.at(i));

            return (!ret.empty()) ? MegaApi::strdup(ret.c_str()) : nullptr;
        }
    }

    return nullptr;
}

const char *MegaChatRoomPrivate::getPeerEmailByHandle(MegaChatHandle userhandle) const
{
    for (unsigned int i = 0; i < peerEmails.size(); i++)
    {
        assert(i < mPeers.size());
        if (mPeers.at(i).first == userhandle)
        {
            return (!peerEmails.at(i).empty()) ? peerEmails.at(i).c_str() : nullptr;
        }
    }

    return nullptr;
}

int MegaChatRoomPrivate::getPeerPrivilege(unsigned int i) const
{
    if (i >= mPeers.size())
    {
        return MegaChatRoom::PRIV_UNKNOWN;
    }

    return mPeers.at(i).second;
}

unsigned int MegaChatRoomPrivate::getPeerCount() const
{
    return mPeers.size();
}

MegaChatHandle MegaChatRoomPrivate::getPeerHandle(unsigned int i) const
{
    if (i >= mPeers.size())
    {
        return MEGACHAT_INVALID_HANDLE;
    }

    return mPeers.at(i).first;
}

const char *MegaChatRoomPrivate::getPeerFirstname(unsigned int i) const
{
    if (i >= peerFirstnames.size())
    {
        return NULL;
    }

    return peerFirstnames.at(i).c_str();
}

const char *MegaChatRoomPrivate::getPeerLastname(unsigned int i) const
{
    if (i >= peerLastnames.size())
    {
        return NULL;
    }

    return peerLastnames.at(i).c_str();
}

const char *MegaChatRoomPrivate::getPeerFullname(unsigned int i) const
{
    if (i >= peerLastnames.size() || i >= peerFirstnames.size())
    {
        return NULL;
    }

    string ret = peerFirstnames.at(i);
    if (!peerFirstnames.at(i).empty() && !peerLastnames.at(i).empty())
    {
        ret.append(" ");
    }
    ret.append(peerLastnames.at(i));

    return MegaApi::strdup(ret.c_str());
}

const char *MegaChatRoomPrivate::getPeerEmail(unsigned int i) const
{
    if (i >= peerEmails.size())
    {
        return NULL;
    }

    return peerEmails.at(i).c_str();
}

bool MegaChatRoomPrivate::isGroup() const
{
    return group;
}

bool MegaChatRoomPrivate::isPublic() const
{
    return mPublicChat;
}

bool MegaChatRoomPrivate::isPreview() const
{
    return mAuthToken.isValid();
}

const char *MegaChatRoomPrivate::getAuthorizationToken() const
{
    if (mAuthToken.isValid())
    {
        return MegaApi::strdup(mAuthToken.toString(Id::CHATLINKHANDLE).c_str());
    }

    return NULL;
}

const char *MegaChatRoomPrivate::getTitle() const
{
    return mTitle.c_str();
}

bool MegaChatRoomPrivate::hasCustomTitle() const
{
    return mHasCustomTitle;
}

bool MegaChatRoomPrivate::isActive() const
{
    return active;
}

bool MegaChatRoomPrivate::isArchived() const
{
    return mArchived;
}

int64_t MegaChatRoomPrivate::getCreationTs() const
{
    return mCreationTs;
}

bool MegaChatRoomPrivate::isMeeting() const
{
    return mMeeting;
}

int MegaChatRoomPrivate::getChanges() const
{
    return mChanged;
}

bool MegaChatRoomPrivate::hasChanged(int changeType) const
{
    return (mChanged & changeType);
}

int MegaChatRoomPrivate::getUnreadCount() const
{
    return unreadCount;
}

MegaChatHandle MegaChatRoomPrivate::getUserHandle() const
{
    return mUh;
}

MegaChatHandle MegaChatRoomPrivate::getUserTyping() const
{
    return mUh;
}

void MegaChatRoomPrivate::setOwnPriv(int ownPriv)
{
    priv = (privilege_t) ownPriv;
    mChanged |= MegaChatRoom::CHANGE_TYPE_OWN_PRIV;
}

void MegaChatRoomPrivate::setTitle(const string& title)
{
    mTitle = title;
    mChanged |= MegaChatRoom::CHANGE_TYPE_TITLE;
}

void MegaChatRoomPrivate::changeUnreadCount()
{
    mChanged |= MegaChatRoom::CHANGE_TYPE_UNREAD_COUNT;
}

void MegaChatRoomPrivate::setNumPreviewers(unsigned int numPrev)
{
    mNumPreviewers = numPrev;
    mChanged |= MegaChatRoom::CHANGE_TYPE_UPDATE_PREVIEWERS;
}

void MegaChatRoomPrivate::setMembersUpdated(MegaChatHandle uh)
{
    mUh = uh;
    mChanged |= MegaChatRoom::CHANGE_TYPE_PARTICIPANTS;
}

void MegaChatRoomPrivate::setUserTyping(MegaChatHandle uh)
{
    mUh = uh;
    mChanged |= MegaChatRoom::CHANGE_TYPE_USER_TYPING;
}

void MegaChatRoomPrivate::setUserStopTyping(MegaChatHandle uh)
{
    mUh = uh;
    mChanged |= MegaChatRoom::CHANGE_TYPE_USER_STOP_TYPING;
}

void MegaChatRoomPrivate::setClosed()
{
    mChanged |= MegaChatRoom::CHANGE_TYPE_CLOSED;
}

void MegaChatRoomPrivate::setChatMode(bool mode)
{
    mPublicChat = mode;
    mChanged |= MegaChatRoom::CHANGE_TYPE_CHAT_MODE;
}

void MegaChatRoomPrivate::setArchived(bool archived)
{
    mArchived = archived;
    mChanged |= MegaChatRoom::CHANGE_TYPE_ARCHIVE;
}

void MegaChatRoomPrivate::setRetentionTime(unsigned int period)
{
    mRetentionTime = period;
    mChanged |= MegaChatRoom::CHANGE_TYPE_RETENTION_TIME;
}

char *MegaChatRoomPrivate::firstnameFromBuffer(const string &buffer)
{
    char *ret = NULL;
    unsigned int len = buffer.length() ? static_cast<unsigned char>(buffer.at(0)) : 0;

    if (len > 0)
    {
        ret = new char[len + 1];
        strncpy(ret, buffer.data() + 1, len);
        ret[len] = '\0';
    }

    return ret;
}

char *MegaChatRoomPrivate::lastnameFromBuffer(const string &buffer)
{
    char *ret = NULL;

    unsigned int firstNameLength = buffer.length() ? static_cast<unsigned char>(buffer.at(0)) : 0;
    if (buffer.length() && (unsigned int)buffer.length() >= firstNameLength)
    {
        int lenLastname = buffer.length() - firstNameLength - 1;
        if (lenLastname)
        {
            const char *start = buffer.data() + 1 + firstNameLength;
            if (buffer.at(0) != 0)
            {
                start++;    // there's a space separator
                lenLastname--;
            }

            ret = new char[lenLastname + 1];
            strncpy(ret, start, lenLastname);
            ret[lenLastname] = '\0';
        }
    }

    return ret;
}

unsigned MegaChatRoomPrivate::getRetentionTime() const
{
    return mRetentionTime;
}

void MegaChatListItemHandler::onTitleChanged(const string &title)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setTitle(title);

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onChatModeChanged(bool mode)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setChatMode(mode);

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onUnreadCountChanged()
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->changeUnreadCount();

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onPreviewersCountUpdate(uint32_t numPrev)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setNumPreviewers(numPrev);

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onPreviewClosed()
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setPreviewClosed();

    chatApi.fireOnChatListItemUpdate(item);
}

MegaChatPeerListPrivate::MegaChatPeerListPrivate()
{
}

MegaChatPeerListPrivate::~MegaChatPeerListPrivate()
{

}

MegaChatPeerList *MegaChatPeerListPrivate::copy() const
{
    MegaChatPeerListPrivate *ret = new MegaChatPeerListPrivate;

    for (int i = 0; i < size(); i++)
    {
        ret->addPeer(list.at(i).first, list.at(i).second);
    }

    return ret;
}

void MegaChatPeerListPrivate::addPeer(MegaChatHandle h, int priv)
{
    list.push_back(userpriv_pair(h, (privilege_t) priv));
}

MegaChatHandle MegaChatPeerListPrivate::getPeerHandle(int i) const
{
    if (i > size())
    {
        return MEGACHAT_INVALID_HANDLE;
    }
    else
    {
        return list.at(i).first;
    }
}

int MegaChatPeerListPrivate::getPeerPrivilege(int i) const
{
    if (i > size())
    {
        return PRIV_UNKNOWN;
    }
    else
    {
        return list.at(i).second;
    }
}

int MegaChatPeerListPrivate::size() const
{
    return list.size();
}

const userpriv_vector *MegaChatPeerListPrivate::getList() const
{
    return &list;
}

MegaChatPeerListPrivate::MegaChatPeerListPrivate(userpriv_vector *userpriv)
{
    handle uh;
    privilege_t priv;

    for (unsigned i = 0; i < userpriv->size(); i++)
    {
        uh = userpriv->at(i).first;
        priv = userpriv->at(i).second;

        addPeer(uh, priv);
    }
}


MegaChatListItemHandler::MegaChatListItemHandler(MegaChatApiImpl &chatApi, ChatRoom &room)
    :chatApi(chatApi), mRoom(room)
{
}

MegaChatListItemPrivate::MegaChatListItemPrivate(ChatRoom &chatroom)
    : MegaChatListItem()
{
    chatid = chatroom.chatid();
    mTitle = chatroom.titleString();
    unreadCount = chatroom.chat().unreadMsgCount();
    group = chatroom.isGroup();
    mPublicChat = chatroom.publicChat();
    mPreviewMode = chatroom.previewMode();
    active = chatroom.isActive();
    mOwnPriv = chatroom.ownPriv();
    mArchived =  chatroom.isArchived();
    mIsCallInProgress = chatroom.isCallActive();
    mChanged = 0;
    peerHandle = !group ? ((PeerChatRoom&)chatroom).peer() : MEGACHAT_INVALID_HANDLE;
    lastMsgPriv = Priv::PRIV_INVALID;
    lastMsgHandle = MEGACHAT_INVALID_HANDLE;
    mNumPreviewers = chatroom.getNumPreviewers();
    mDeleted = false;

    LastTextMsg tmp;
    LastTextMsg *message = &tmp;
    LastTextMsg *&msg = message;
    uint8_t lastMsgStatus = chatroom.chat().lastTextMessage(msg);
    if (lastMsgStatus == LastTextMsgState::kHave)
    {
        lastMsgSender = msg->sender();
        lastMsgType = msg->type();
        mLastMsgId = (msg->idx() == CHATD_IDX_INVALID) ? msg->xid() : msg->id();

        switch (lastMsgType)
        {
            case MegaChatMessage::TYPE_CONTACT_ATTACHMENT:
            case MegaChatMessage::TYPE_NODE_ATTACHMENT:
            case MegaChatMessage::TYPE_CONTAINS_META:
            case MegaChatMessage::TYPE_VOICE_CLIP:
                lastMsg = JSonUtils::getLastMessageContent(msg->contents(), msg->type());
                break;

            case MegaChatMessage::TYPE_ALTER_PARTICIPANTS:
            case MegaChatMessage::TYPE_PRIV_CHANGE:
            {
                const Message::ManagementInfo *management = reinterpret_cast<const Message::ManagementInfo*>(msg->contents().data());
                lastMsgPriv = management->privilege;
                lastMsgHandle = (MegaChatHandle)management->target;
                break;
            }

            case MegaChatMessage::TYPE_NORMAL:
            case MegaChatMessage::TYPE_CHAT_TITLE:
                lastMsg = msg->contents();
                break;

            case MegaChatMessage::TYPE_CALL_ENDED:
            {
                Message::CallEndedInfo *callEndedInfo = Message::CallEndedInfo::fromBuffer(msg->contents().data(), msg->contents().size());
                if (callEndedInfo)
                {
                    lastMsg = std::to_string(callEndedInfo->duration);
                    lastMsg.push_back(0x01);
                    int termCode = MegaChatMessagePrivate::convertEndCallTermCodeToUI(*callEndedInfo);
                    lastMsg += std::to_string(termCode);
                    for (unsigned int i = 0; i < callEndedInfo->participants.size(); i++)
                    {
                        lastMsg.push_back(0x01);
                        karere::Id id(callEndedInfo->participants[i]);
                        lastMsg += id.toString();
                    }
                    delete callEndedInfo;
                }
                break;
            }

            case MegaChatMessage::TYPE_SET_RETENTION_TIME:
            {
               uint32_t retentionTime;
               memcpy(&retentionTime, msg->contents().c_str(), msg->contents().size());
               lastMsg = std::to_string(retentionTime);
               break;
            }

            case MegaChatMessage::TYPE_REVOKE_NODE_ATTACHMENT:  // deprecated: should not be notified as last-message
            case MegaChatMessage::TYPE_TRUNCATE:                // no content at all
            case MegaChatMessage::TYPE_CALL_STARTED:            // no content at all
            case MegaChatMessage::TYPE_PUBLIC_HANDLE_CREATE:    // no content at all
            case MegaChatMessage::TYPE_PUBLIC_HANDLE_DELETE:    // no content at all
            case MegaChatMessage::TYPE_SET_PRIVATE_MODE:
            case MegaChatRequest::TYPE_LOAD_AUDIO_VIDEO_DEVICES:
            default:
                break;
        }
    }
    else
    {
        lastMsg = "";
        lastMsgSender = MEGACHAT_INVALID_HANDLE;
        lastMsgType = lastMsgStatus;
        mLastMsgId = MEGACHAT_INVALID_HANDLE;
    }

    lastTs = chatroom.chat().lastMessageTs();
}

MegaChatListItemPrivate::MegaChatListItemPrivate(const MegaChatListItem *item)
{
    chatid = item->getChatId();
    mTitle = item->getTitle();
    mOwnPriv = item->getOwnPrivilege();
    unreadCount = item->getUnreadCount();
    mChanged = item->getChanges();
    lastTs = item->getLastTimestamp();
    lastMsg = item->getLastMessage();
    lastMsgType = item->getLastMessageType();
    lastMsgSender = item->getLastMessageSender();
    group = item->isGroup();
    mPublicChat = item->isPublic();
    mPreviewMode = item->isPreview();
    active = item->isActive();
    peerHandle = item->getPeerHandle();
    mLastMsgId = item->getLastMessageId();
    mArchived = item->isArchived();
    mIsCallInProgress = item->isCallInProgress();
    lastMsgPriv = item->getLastMessagePriv();
    lastMsgHandle = item->getLastMessageHandle();
    mNumPreviewers = item->getNumPreviewers();
    mDeleted = item->isDeleted();
}

MegaChatListItemPrivate::~MegaChatListItemPrivate()
{
}

MegaChatListItem *MegaChatListItemPrivate::copy() const
{
    return new MegaChatListItemPrivate(this);
}

int MegaChatListItemPrivate::getChanges() const
{
    return mChanged;
}

bool MegaChatListItemPrivate::hasChanged(int changeType) const
{
    return (mChanged & changeType);
}

MegaChatHandle MegaChatListItemPrivate::getChatId() const
{
    return chatid;
}

const char *MegaChatListItemPrivate::getTitle() const
{
    return mTitle.c_str();
}

int MegaChatListItemPrivate::getOwnPrivilege() const
{
    return mOwnPriv;
}

int MegaChatListItemPrivate::getUnreadCount() const
{
    return unreadCount;
}

const char *MegaChatListItemPrivate::getLastMessage() const
{
    return lastMsg.c_str();
}

MegaChatHandle MegaChatListItemPrivate::getLastMessageId() const
{
    return mLastMsgId;
}

int MegaChatListItemPrivate::getLastMessageType() const
{
    return lastMsgType;
}

MegaChatHandle MegaChatListItemPrivate::getLastMessageSender() const
{
    return lastMsgSender;
}

int64_t MegaChatListItemPrivate::getLastTimestamp() const
{
    return lastTs;
}

bool MegaChatListItemPrivate::isGroup() const
{
    return group;
}

bool MegaChatListItemPrivate::isPublic() const
{
    return mPublicChat;
}

bool MegaChatListItemPrivate::isPreview() const
{
    return mPreviewMode;
}

bool MegaChatListItemPrivate::isActive() const
{
    return active;
}

bool MegaChatListItemPrivate::isArchived() const
{
    return mArchived;
}

bool MegaChatListItemPrivate::isDeleted() const
{
    return mDeleted;
}

bool MegaChatListItemPrivate::isCallInProgress() const
{
    return mIsCallInProgress;
}

MegaChatHandle MegaChatListItemPrivate::getPeerHandle() const
{
    return peerHandle;
}

int MegaChatListItemPrivate::getLastMessagePriv() const
{
    return lastMsgPriv;
}

MegaChatHandle MegaChatListItemPrivate::getLastMessageHandle() const
{
    return lastMsgHandle;
}

unsigned int MegaChatListItemPrivate::getNumPreviewers() const
{
   return mNumPreviewers;
}

void MegaChatListItemPrivate::setOwnPriv(int ownPriv)
{
    mOwnPriv = ownPriv;
    mChanged |= MegaChatListItem::CHANGE_TYPE_OWN_PRIV;
}

void MegaChatListItemPrivate::setTitle(const string &title)
{
    mTitle = title;
    mChanged |= MegaChatListItem::CHANGE_TYPE_TITLE;
}

void MegaChatListItemPrivate::changeUnreadCount()
{
    mChanged |= MegaChatListItem::CHANGE_TYPE_UNREAD_COUNT;
}

void MegaChatListItemPrivate::setNumPreviewers(unsigned int numPrev)
{
    mNumPreviewers = numPrev;
    mChanged |= MegaChatListItem::CHANGE_TYPE_UPDATE_PREVIEWERS;
}

void MegaChatListItemPrivate::setPreviewClosed()
{
    mChanged |= MegaChatListItem::CHANGE_TYPE_PREVIEW_CLOSED;
}

void MegaChatListItemPrivate::setMembersUpdated()
{
    mChanged |= MegaChatListItem::CHANGE_TYPE_PARTICIPANTS;
}

void MegaChatListItemPrivate::setClosed()
{
    mChanged |= MegaChatListItem::CHANGE_TYPE_CLOSED;
}

void MegaChatListItemPrivate::setLastTimestamp(int64_t ts)
{
    lastTs = ts;
    mChanged |= MegaChatListItem::CHANGE_TYPE_LAST_TS;
}

void MegaChatListItemPrivate::setArchived(bool archived)
{
    mArchived = archived;
    mChanged |= MegaChatListItem::CHANGE_TYPE_ARCHIVE;
}

void MegaChatListItemPrivate::setDeleted()
{
    mDeleted = true;
    mChanged |= MegaChatListItem::CHANGE_TYPE_DELETED;
}

void MegaChatListItemPrivate::setCallInProgress()
{
    mChanged |= MegaChatListItem::CHANGE_TYPE_CALL;
}

void MegaChatListItemPrivate::setLastMessage()
{
    mChanged |= MegaChatListItem::CHANGE_TYPE_LAST_MSG;
}

void MegaChatListItemPrivate::setChatMode(bool mode)
{
    mPublicChat = mode;
    mChanged |= MegaChatListItem::CHANGE_TYPE_CHAT_MODE;
}

MegaChatGroupListItemHandler::MegaChatGroupListItemHandler(MegaChatApiImpl &chatApi, ChatRoom &room)
    : MegaChatListItemHandler(chatApi, room)
{

}

void MegaChatGroupListItemHandler::onUserJoin(uint64_t userid, Priv priv)
{
    bool ownChange = (userid == chatApi.getMyUserHandle());

    // avoid to notify if own user doesn't participate or isn't online and it's a public chat (for large chat-links, for performance)
    if (!ownChange && mRoom.publicChat() && (mRoom.chat().onlineState() != kChatStateOnline || mRoom.chat().getOwnprivilege() == chatd::Priv::PRIV_NOTPRESENT))
    {
        return;
    }

    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    if (ownChange)
    {
        item->setOwnPriv(priv);
    }
    else
    {
        item->setMembersUpdated();
    }

    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatGroupListItemHandler::onUserLeave(uint64_t )
{
    if (mRoom.publicChat() && mRoom.chat().getOwnprivilege() == chatd::Priv::PRIV_NOTPRESENT)
    {
        return;
    }

    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setMembersUpdated();
    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onExcludedFromChat()
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setOwnPriv(item->getOwnPrivilege());
    item->setClosed();
    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onRejoinedChat()
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setOwnPriv(item->getOwnPrivilege());
    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onLastMessageUpdated(const LastTextMsg& /*msg*/)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setLastMessage();
    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onLastTsUpdated(uint32_t ts)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setLastTimestamp(ts);
    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onChatOnlineState(const ChatState state)
{
    int newState = MegaChatApiImpl::convertChatConnectionState(state);
    chatApi.fireOnChatConnectionStateUpdate(mRoom.chatid(), newState);
}

void MegaChatListItemHandler::onChatArchived(bool archived)
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setArchived(archived);
    chatApi.fireOnChatListItemUpdate(item);
}

void MegaChatListItemHandler::onChatDeleted() const
{
    MegaChatListItemPrivate *item = new MegaChatListItemPrivate(mRoom);
    item->setDeleted();

    chatApi.fireOnChatListItemUpdate(item);
}

MegaChatPeerListItemHandler::MegaChatPeerListItemHandler(MegaChatApiImpl &chatApi, ChatRoom &room)
    : MegaChatListItemHandler(chatApi, room)
{

}

MegaChatMessagePrivate::MegaChatMessagePrivate(const MegaChatMessage *msg)
{
    mMsg = MegaApi::strdup(msg->getContent());
    uh = msg->getUserHandle();
    hAction = msg->getHandleOfAction();
    msgId = msg->getMsgId();
    mTempId = msg->getTempId();
    mIndex = msg->getMsgIndex();
    mStatus = msg->getStatus();
    ts = msg->getTimestamp();
    type = msg->getType();
    mHasReactions = msg->hasConfirmedReactions();
    changed = msg->getChanges();
    edited = msg->isEdited();
    deleted = msg->isDeleted();
    priv = msg->getPrivilege();
    mCode = msg->getCode();
    rowId = msg->getRowId();
    megaNodeList = msg->getMegaNodeList() ? msg->getMegaNodeList()->copy() : NULL;
    megaHandleList = msg->getMegaHandleList() ? msg->getMegaHandleList()->copy() : NULL;

    if (msg->getUsersCount() != 0)
    {
        megaChatUsers = new std::vector<MegaChatAttachedUser>();

        for (unsigned int i = 0; i < msg->getUsersCount(); ++i)
        {
            MegaChatAttachedUser megaChatUser(msg->getUserHandle(i), msg->getUserEmail(i), msg->getUserName(i));

            megaChatUsers->push_back(megaChatUser);
        }
    }

    if (msg->getType() == TYPE_CONTAINS_META)
    {
        mContainsMeta = msg->getContainsMeta()->copy();
    }
}

MegaChatMessagePrivate::MegaChatMessagePrivate(const Message &msg, Message::Status status, Idx index)
{
    if (msg.type == TYPE_NORMAL || msg.type == TYPE_CHAT_TITLE)
    {
        string tmp(msg.buf(), msg.size());
        mMsg = msg.size() ? MegaApi::strdup(tmp.c_str()) : NULL;
    }
    else    // for other types, content is irrelevant
    {
        mMsg = NULL;
    }
    uh = msg.userid;
    msgId = msg.isSending() ? MEGACHAT_INVALID_HANDLE : (MegaChatHandle) msg.id();
    mTempId = msg.isSending() ? (MegaChatHandle) msg.id() : MEGACHAT_INVALID_HANDLE;
    rowId = MEGACHAT_INVALID_HANDLE;
    type = msg.type;
    mHasReactions = msg.hasConfirmedReactions();
    ts = msg.ts;
    mStatus = status;
    mIndex = index;
    changed = 0;
    edited = msg.updated && msg.size();
    deleted = msg.updated && !msg.size();
    mCode = 0;
    priv = PRIV_UNKNOWN;
    hAction = MEGACHAT_INVALID_HANDLE;

    switch (type)
    {
        case MegaChatMessage::TYPE_PRIV_CHANGE:
        case MegaChatMessage::TYPE_ALTER_PARTICIPANTS:
        {
            const Message::ManagementInfo mngInfo = msg.mgmtInfo();

            priv = mngInfo.privilege;
            hAction = mngInfo.target;
            break;
        }
        case MegaChatMessage::TYPE_NODE_ATTACHMENT:
        case MegaChatMessage::TYPE_VOICE_CLIP:
        {
            megaNodeList = JSonUtils::parseAttachNodeJSon(msg.toText().c_str());
            break;
        }
        case MegaChatMessage::TYPE_REVOKE_NODE_ATTACHMENT:
        {
            hAction = MegaApi::base64ToHandle(msg.toText().c_str());
            break;
        }
        case MegaChatMessage::TYPE_CONTACT_ATTACHMENT:
        {
            megaChatUsers = JSonUtils::parseAttachContactJSon(msg.toText().c_str());
            break;
        }
        case MegaChatMessage::TYPE_CONTAINS_META:
        {
            uint8_t containsMetaType = msg.containMetaSubtype();
            string containsMetaJson = msg.containsMetaJson();
            mContainsMeta = JSonUtils::parseContainsMeta(containsMetaJson.c_str(), containsMetaType);
            break;
        }
        case MegaChatMessage::TYPE_CALL_ENDED:
        {
            megaHandleList = new MegaHandleListPrivate();
            Message::CallEndedInfo *callEndInfo = Message::CallEndedInfo::fromBuffer(msg.buf(), msg.size());
            if (callEndInfo)
            {
                for (size_t i = 0; i < callEndInfo->participants.size(); i++)
                {
                    megaHandleList->addMegaHandle(callEndInfo->participants[i]);
                }

                priv = callEndInfo->duration;
                mCode = MegaChatMessagePrivate::convertEndCallTermCodeToUI(*callEndInfo);
                delete callEndInfo;
            }
            break;
        }

        case MegaChatMessage::TYPE_SET_RETENTION_TIME:
        {
          // Interpret retentionTime as int32_t to store it in an existing member.
          assert(sizeof(priv) == msg.dataSize());
          memcpy(&priv, msg.buf(), min(sizeof(priv), msg.dataSize()));
          break;
        }

        case MegaChatMessage::TYPE_NORMAL:
        case MegaChatMessage::TYPE_CHAT_TITLE:
        case MegaChatMessage::TYPE_TRUNCATE:
        case MegaChatMessage::TYPE_CALL_STARTED:
        case MegaChatMessage::TYPE_PUBLIC_HANDLE_CREATE:
        case MegaChatMessage::TYPE_PUBLIC_HANDLE_DELETE:
        case MegaChatMessage::TYPE_SET_PRIVATE_MODE:
            break;
        default:
        {
            type = MegaChatMessage::TYPE_UNKNOWN;
            break;
        }
    }

    int encryptionState = msg.isEncrypted();
    switch (encryptionState)
    {
    case Message::kEncryptedPending:    // transient, app will receive update once decrypted
    case Message::kEncryptedNoKey:
    case Message::kEncryptedNoType:
        mCode = encryptionState;
        type = MegaChatMessage::TYPE_UNKNOWN; // --> ignore/hide them
        break;
    case Message::kEncryptedMalformed:
    case Message::kEncryptedSignature:
        mCode = encryptionState;
        type = MegaChatMessage::TYPE_INVALID; // --> show a warning
        break;
    case Message::kNotEncrypted:
        break;
    }
}

MegaChatMessagePrivate::~MegaChatMessagePrivate()
{
    delete [] mMsg;
    delete megaChatUsers;
    delete megaNodeList;
    delete mContainsMeta;
    delete megaHandleList;
}

MegaChatMessage *MegaChatMessagePrivate::copy() const
{
    return new MegaChatMessagePrivate(this);
}

int MegaChatMessagePrivate::getStatus() const
{
    return mStatus;
}

MegaChatHandle MegaChatMessagePrivate::getMsgId() const
{
    return msgId;
}

MegaChatHandle MegaChatMessagePrivate::getTempId() const
{
    return mTempId;
}

int MegaChatMessagePrivate::getMsgIndex() const
{
    return mIndex;
}

MegaChatHandle MegaChatMessagePrivate::getUserHandle() const
{
    return uh;
}

int MegaChatMessagePrivate::getType() const
{
    return type;
}

bool MegaChatMessagePrivate::hasConfirmedReactions() const
{
    return mHasReactions;
}

int64_t MegaChatMessagePrivate::getTimestamp() const
{
    return ts;
}

const char *MegaChatMessagePrivate::getContent() const
{
    // if message contains meta and is of rich-link type, return the original content
    if (type == MegaChatMessage::TYPE_CONTAINS_META)
    {
        return getContainsMeta()->getTextMessage();

    }
    return mMsg;
}

bool MegaChatMessagePrivate::isEdited() const
{
    return edited;
}

bool MegaChatMessagePrivate::isDeleted() const
{
    return deleted;
}

bool MegaChatMessagePrivate::isEditable() const
{
    return ((type == TYPE_NORMAL || type == TYPE_CONTAINS_META) && !isDeleted() && ((time(NULL) - ts) < CHATD_MAX_EDIT_AGE) && !isGiphy());
}

bool MegaChatMessagePrivate::isDeletable() const
{
    return ((type == TYPE_NORMAL || type == TYPE_CONTACT_ATTACHMENT || type == TYPE_NODE_ATTACHMENT || type == TYPE_CONTAINS_META || type == TYPE_VOICE_CLIP)
            && !isDeleted() && ((time(NULL) - ts) < CHATD_MAX_EDIT_AGE));
}

bool MegaChatMessagePrivate::isManagementMessage() const
{
    return (type >= TYPE_LOWEST_MANAGEMENT
            && type <= TYPE_HIGHEST_MANAGEMENT);
}

MegaChatHandle MegaChatMessagePrivate::getHandleOfAction() const
{
    return hAction;
}

int MegaChatMessagePrivate::getPrivilege() const
{
    return priv;
}

int MegaChatMessagePrivate::getCode() const
{
    return mCode;
}

MegaChatHandle MegaChatMessagePrivate::getRowId() const
{
    return rowId;
}

int MegaChatMessagePrivate::getChanges() const
{
    return changed;
}

bool MegaChatMessagePrivate::hasChanged(int changeType) const
{
    return (changed & changeType);
}

void MegaChatMessagePrivate::setStatus(int status)
{
    mStatus = status;
    changed |= MegaChatMessage::CHANGE_TYPE_STATUS;
}

void MegaChatMessagePrivate::setTempId(MegaChatHandle tempId)
{
    mTempId = tempId;
}

void MegaChatMessagePrivate::setRowId(int id)
{
    rowId = id;
}

void MegaChatMessagePrivate::setContentChanged()
{
    changed |= MegaChatMessage::CHANGE_TYPE_CONTENT;
}

void MegaChatMessagePrivate::setCode(int code)
{
    mCode = code;
}

void MegaChatMessagePrivate::setAccess()
{
    changed |= MegaChatMessage::CHANGE_TYPE_ACCESS;
}

void MegaChatMessagePrivate::setTsUpdated()
{
    changed |= MegaChatMessage::CHANGE_TYPE_TIMESTAMP;
}

int MegaChatMessagePrivate::convertEndCallTermCodeToUI(const Message::CallEndedInfo  &callEndInfo)
{
    int code;
    switch (callEndInfo.termCode)
    {
        case END_CALL_REASON_ENDED:;
        case END_CALL_REASON_FAILED:
            if (callEndInfo.duration > 0)
            {
                code =  END_CALL_REASON_ENDED;
            }
            else
            {
                code = END_CALL_REASON_FAILED;
            }
            break;
        default:
            code = callEndInfo.termCode;
            break;
    }

    return code;
}

unsigned int MegaChatMessagePrivate::getUsersCount() const
{
    unsigned int size = 0;
    if (megaChatUsers != NULL)
    {
        size = megaChatUsers->size();
    }

    return size;
}

MegaChatHandle MegaChatMessagePrivate::getUserHandle(unsigned int index) const
{
    if (!megaChatUsers || index >= megaChatUsers->size())
    {
        return MEGACHAT_INVALID_HANDLE;
    }

    return megaChatUsers->at(index).getHandle();
}

const char *MegaChatMessagePrivate::getUserName(unsigned int index) const
{
    if (!megaChatUsers || index >= megaChatUsers->size())
    {
        return NULL;
    }

    return megaChatUsers->at(index).getName();
}

const char *MegaChatMessagePrivate::getUserEmail(unsigned int index) const
{
    if (!megaChatUsers || index >= megaChatUsers->size())
    {
        return NULL;
    }

    return megaChatUsers->at(index).getEmail();
}

MegaNodeList *MegaChatMessagePrivate::getMegaNodeList() const
{
    return megaNodeList;
}

const MegaChatContainsMeta *MegaChatMessagePrivate::getContainsMeta() const
{
    return mContainsMeta;
}

MegaHandleList *MegaChatMessagePrivate::getMegaHandleList() const
{
    return megaHandleList;
}

int MegaChatMessagePrivate::getDuration() const
{
    return priv;
}

unsigned MegaChatMessagePrivate::getRetentionTime() const
{
    return static_cast<unsigned>(priv);
}

int MegaChatMessagePrivate::getTermCode() const
{
    return mCode;
}

bool MegaChatMessagePrivate::isGiphy() const
{
    if (auto metaType = getContainsMeta())
    {
        return metaType->getType() == MegaChatContainsMeta::CONTAINS_META_GIPHY;
    }
    return false;
}

LoggerHandler::LoggerHandler()
    : ILoggerBackend(MegaChatApi::LOG_LEVEL_INFO)
{
    megaLogger = NULL;

    gLogger.addUserLogger("MegaChatApi", this);
    gLogger.logChannels[krLogChannel_megasdk].logLevel = krLogLevelDebugVerbose;
    gLogger.logChannels[krLogChannel_websockets].logLevel = krLogLevelDebugVerbose;
    gLogger.logToConsoleUseColors(false);
}

LoggerHandler::~LoggerHandler()
{
    gLogger.removeUserLogger("MegaChatApi");
}

void LoggerHandler::setMegaChatLogger(MegaChatLogger *logger)
{
    mutex.lock();
    megaLogger = logger;
    mutex.unlock();
}

void LoggerHandler::setLogLevel(int logLevel)
{
    mutex.lock();
    maxLogLevel = logLevel;
    switch (logLevel)
    {
        case MegaChatApi::LOG_LEVEL_ERROR:
            MegaApi::setLogLevel(MegaApi::LOG_LEVEL_ERROR);
            break;

        case MegaChatApi::LOG_LEVEL_WARNING:
            MegaApi::setLogLevel(MegaApi::LOG_LEVEL_WARNING);
            break;

        case MegaChatApi::LOG_LEVEL_INFO:
            MegaApi::setLogLevel(MegaApi::LOG_LEVEL_INFO);
            break;

        case MegaChatApi::LOG_LEVEL_VERBOSE:
        case MegaChatApi::LOG_LEVEL_DEBUG:
            MegaApi::setLogLevel(MegaApi::LOG_LEVEL_DEBUG);
            break;

        case MegaChatApi::LOG_LEVEL_MAX:
            MegaApi::setLogLevel(MegaApi::LOG_LEVEL_MAX);
            break;

        default:
            break;
    }
    mutex.unlock();
}

void LoggerHandler::setLogWithColors(bool useColors)
{
    gLogger.logToConsoleUseColors(useColors);
}

void LoggerHandler::setLogToConsole(bool enable)
{
    gLogger.logToConsole(enable);
}

void LoggerHandler::log(krLogLevel level, const char *msg, size_t /*len*/, unsigned /*flags*/)
{
    mutex.lock();
    if (megaLogger)
    {
        megaLogger->log(level, msg);
    }
    mutex.unlock();
}

#ifndef KARERE_DISABLE_WEBRTC

MegaChatCallHandler::MegaChatCallHandler(MegaChatApiImpl *megaChatApi)
{
    mMegaChatApi = megaChatApi;
}

MegaChatCallHandler::~MegaChatCallHandler()
{
}

void MegaChatCallHandler::onCallStateChange(rtcModule::ICall &call)
{
    std::unique_ptr<MegaChatCallPrivate> chatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    chatCall->setStatus(MegaChatCallPrivate::convertCallState(call.getState()));
    mMegaChatApi->fireOnChatCallUpdate(chatCall.get());
}

void MegaChatCallHandler::onCallRinging(rtcModule::ICall &call)
{
    std::unique_ptr<MegaChatCallPrivate> chatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    chatCall->setChange(MegaChatCall::CHANGE_TYPE_RINGING_STATUS);
    mMegaChatApi->fireOnChatCallUpdate(chatCall.get());
}

void MegaChatCallHandler::onNewSession(rtcModule::ISession& sess, const rtcModule::ICall &call)
{
    MegaChatSessionHandler *sessionHandler = new MegaChatSessionHandler(mMegaChatApi, call);
    sess.setSessionHandler(sessionHandler); // takes ownership, destroyed after onDestroySession()

    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(sess);
    megaSession->setChange(MegaChatSession::CHANGE_TYPE_STATUS);
    mMegaChatApi->fireOnChatSessionUpdate(call.getChatid(), call.getCallid(), megaSession.get());
}

void MegaChatCallHandler::onAudioApproved(const rtcModule::ICall &call)
{
    std::unique_ptr<MegaChatCallPrivate> chatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    chatCall->setChange(MegaChatCall::CHANGE_TYPE_CALL_SPEAK);
    mMegaChatApi->fireOnChatCallUpdate(chatCall.get());
}

void MegaChatCallHandler::onLocalFlagsChanged(const rtcModule::ICall &call)
{
    std::unique_ptr<MegaChatCallPrivate> chatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    chatCall->setChange(MegaChatCall::CHANGE_TYPE_LOCAL_AVFLAGS);
    mMegaChatApi->fireOnChatCallUpdate(chatCall.get());
}

void MegaChatCallHandler::onLocalAudioDetected(const rtcModule::ICall& call)
{
    std::unique_ptr<MegaChatCallPrivate> megaChatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    megaChatCall->setAudioDetected(call.isAudioDetected());
    mMegaChatApi->fireOnChatCallUpdate(megaChatCall.get());
}

void MegaChatCallHandler::onOnHold(const rtcModule::ICall& call)
{
    std::unique_ptr<MegaChatCallPrivate> chatCall = ::mega::make_unique<MegaChatCallPrivate>(call);
    chatCall->setOnHold(call.getLocalAvFlags().isOnHold());
    mMegaChatApi->fireOnChatCallUpdate(chatCall.get());
}

MegaChatSessionHandler::MegaChatSessionHandler(MegaChatApiImpl *megaChatApi, const rtcModule::ICall& call)
{
    mMegaChatApi = megaChatApi;
    mChatid = call.getChatid();
    mCallid = call.getCallid();
}

MegaChatSessionHandler::~MegaChatSessionHandler()
{
}

void MegaChatSessionHandler::onSpeakRequest(rtcModule::ISession &session, bool /*requested*/)
{
    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setChange(MegaChatSession::CHANGE_TYPE_SESSION_SPEAK_REQUESTED);
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}

void MegaChatSessionHandler::onVThumbReceived(rtcModule::ISession& session)
{
    if (session.hasLowResolutionTrack())
    {
        session.setVideoRendererVthumb(new MegaChatVideoReceiver(mMegaChatApi, mChatid, rtcModule::VideoResolution::kLowRes, session.getClientid()));
    }
    else
    {
        session.setVideoRendererVthumb(nullptr);
    }

    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setChange(MegaChatSession::CHANGE_TYPE_SESSION_ON_LOWRES);
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}

void MegaChatSessionHandler::onHiResReceived(rtcModule::ISession& session)
{
    if (session.hasHighResolutionTrack())
    {
        session.setVideoRendererHiRes(new MegaChatVideoReceiver(mMegaChatApi, mChatid, rtcModule::VideoResolution::kHiRes, session.getClientid()));
    }
    else
    {
        session.setVideoRendererHiRes(nullptr);
    }

    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setChange(MegaChatSession::CHANGE_TYPE_SESSION_ON_HIRES);
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}

void MegaChatSessionHandler::onDestroySession(rtcModule::ISession &session)
{
    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setChange(MegaChatSession::CHANGE_TYPE_STATUS);
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}

void MegaChatSessionHandler::onAudioRequested(rtcModule::ISession &session)
{
    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setChange(MegaChatSession::CHANGE_TYPE_SESSION_SPEAK_REQUESTED);
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}

void MegaChatSessionHandler::onRemoteFlagsChanged(rtcModule::ISession &session)
{
    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setChange(MegaChatSession::CHANGE_TYPE_REMOTE_AVFLAGS);
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}

void MegaChatSessionHandler::onOnHold(rtcModule::ISession& session)
{
    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setOnHold(session.getAvFlags().isOnHold());
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}

void MegaChatSessionHandler::onRemoteAudioDetected(rtcModule::ISession& session)
{
    std::unique_ptr<MegaChatSessionPrivate> megaSession = ::mega::make_unique<MegaChatSessionPrivate>(session);
    megaSession->setAudioDetected(session.isAudioDetected());
    mMegaChatApi->fireOnChatSessionUpdate(mChatid, mCallid, megaSession.get());
}
#endif

MegaChatListItemListPrivate::MegaChatListItemListPrivate()
{
}

MegaChatListItemListPrivate::~MegaChatListItemListPrivate()
{
    for (unsigned int i = 0; i < mList.size(); i++)
    {
        delete mList[i];
        mList[i] = NULL;
    }

    mList.clear();
}

MegaChatListItemListPrivate::MegaChatListItemListPrivate(const MegaChatListItemListPrivate *list)
{
    MegaChatListItemPrivate *item;

    for (unsigned int i = 0; i < list->size(); i++)
    {
        item = new MegaChatListItemPrivate(list->get(i));
        mList.push_back(item);
    }
}

MegaChatListItemListPrivate *MegaChatListItemListPrivate::copy() const
{
    return new MegaChatListItemListPrivate(this);
}

const MegaChatListItem *MegaChatListItemListPrivate::get(unsigned int i) const
{
    if (i >= size())
    {
        return NULL;
    }
    else
    {
        return mList.at(i);
    }
}

unsigned int MegaChatListItemListPrivate::size() const
{
    return mList.size();
}

void MegaChatListItemListPrivate::addChatListItem(MegaChatListItem *item)
{
    mList.push_back(item);
}

MegaChatPresenceConfigPrivate::MegaChatPresenceConfigPrivate(const MegaChatPresenceConfigPrivate &config)
{
    status = config.getOnlineStatus();
    autoawayEnabled = config.isAutoawayEnabled();
    autoawayTimeout = config.getAutoawayTimeout();
    persistEnabled = config.isPersist();
    lastGreenVisible = config.isLastGreenVisible();
    pending = config.isPending();
}

MegaChatPresenceConfigPrivate::MegaChatPresenceConfigPrivate(const presenced::Config &config, bool isPending)
{
    status = config.presence().status();
    autoawayEnabled = config.autoawayActive();
    autoawayTimeout = config.autoawayTimeout();
    persistEnabled = config.persist();
    lastGreenVisible = config.lastGreenVisible();
    pending = isPending;
}

MegaChatPresenceConfigPrivate::~MegaChatPresenceConfigPrivate()
{

}

MegaChatPresenceConfig *MegaChatPresenceConfigPrivate::copy() const
{
    return new MegaChatPresenceConfigPrivate(*this);
}

int MegaChatPresenceConfigPrivate::getOnlineStatus() const
{
    return status;
}

bool MegaChatPresenceConfigPrivate::isAutoawayEnabled() const
{
    return autoawayEnabled;
}

int64_t MegaChatPresenceConfigPrivate::getAutoawayTimeout() const
{
    return autoawayTimeout;
}

bool MegaChatPresenceConfigPrivate::isPersist() const
{
    return persistEnabled;
}

bool MegaChatPresenceConfigPrivate::isPending() const
{
    return pending;
}

bool MegaChatPresenceConfigPrivate::isLastGreenVisible() const
{
    return lastGreenVisible;
}

MegaChatAttachedUser::MegaChatAttachedUser(MegaChatHandle contactId, const std::string &email, const std::string& name)
    : mHandle(contactId)
    , mEmail(email)
    , mName(name)
{
}

MegaChatAttachedUser::~MegaChatAttachedUser()
{
}

MegaChatHandle MegaChatAttachedUser::getHandle() const
{
    return mHandle;
}

const char *MegaChatAttachedUser::getEmail() const
{
    return mEmail.c_str();
}

const char *MegaChatAttachedUser::getName() const
{
    return mName.c_str();
}

int MegaChatContainsMetaPrivate::getType() const
{
    return mType;
}

const char *MegaChatContainsMetaPrivate::getTextMessage() const
{
    return mText.c_str();
}

const MegaChatRichPreview *MegaChatContainsMetaPrivate::getRichPreview() const
{
    return mRichPreview;
}

const MegaChatGeolocation *MegaChatContainsMetaPrivate::getGeolocation() const
{
    return mGeolocation;
}

const MegaChatGiphy* MegaChatContainsMetaPrivate::getGiphy() const
{
    return mGiphy.get();
}

void MegaChatContainsMetaPrivate::setRichPreview(MegaChatRichPreview *richPreview)
{
    if (mRichPreview)
    {
        delete mRichPreview;
    }

    if (richPreview)
    {
        mType = MegaChatContainsMeta::CONTAINS_META_RICH_PREVIEW;
        mRichPreview = richPreview;
    }
    else
    {
        mType = MegaChatContainsMeta::CONTAINS_META_INVALID;
        mRichPreview = NULL;
    }
}

void MegaChatContainsMetaPrivate::setGeolocation(MegaChatGeolocation *geolocation)
{
    if (mGeolocation)
    {
        delete mGeolocation;
    }

    if (geolocation)
    {
        mType = MegaChatContainsMeta::CONTAINS_META_GEOLOCATION;
        mGeolocation = geolocation;
    }
    else
    {
        mType = MegaChatContainsMeta::CONTAINS_META_INVALID;
        mGeolocation = NULL;
    }
}

void MegaChatContainsMetaPrivate::setTextMessage(const string &text)
{
    mText = text;
}

void MegaChatContainsMetaPrivate::setGiphy(std::unique_ptr<MegaChatGiphy> giphy)
{
    if (giphy)
    {
        mType = MegaChatContainsMeta::CONTAINS_META_GIPHY;
        mGiphy = std::move(giphy);
    }
    else
    {
        mType = MegaChatContainsMeta::CONTAINS_META_INVALID;
        mGiphy.reset(nullptr);
    }
}

MegaChatContainsMetaPrivate::MegaChatContainsMetaPrivate(const MegaChatContainsMeta *containsMeta)
{
    if (!containsMeta)
    {
        return;
    }

    mType = containsMeta->getType();
    mRichPreview = containsMeta->getRichPreview() ? containsMeta->getRichPreview()->copy() : NULL;
    mGeolocation = containsMeta->getGeolocation() ? containsMeta->getGeolocation()->copy() : NULL;
    mGiphy = std::unique_ptr<MegaChatGiphy>(containsMeta->getGiphy() ? containsMeta->getGiphy()->copy() : nullptr);

    mText = containsMeta->getTextMessage();
}

MegaChatContainsMetaPrivate::~MegaChatContainsMetaPrivate()
{
    delete mRichPreview;
    delete mGeolocation;
}

MegaChatContainsMeta *MegaChatContainsMetaPrivate::copy() const
{
    return new MegaChatContainsMetaPrivate(this);
}

MegaChatRichPreviewPrivate::MegaChatRichPreviewPrivate(const MegaChatRichPreview *richPreview)
{
    mText = richPreview->getText();
    mTitle = richPreview->getTitle();
    mDescription = richPreview->getDescription();
    mImage = richPreview->getImage() ? richPreview->getImage() : "";
    mImageFormat = richPreview->getImageFormat();
    mIcon = richPreview->getIcon() ? richPreview->getIcon() : "";
    mIconFormat = richPreview->getIconFormat();
    mUrl = richPreview->getUrl();
    mDomainName = richPreview->getDomainName();
}

MegaChatRichPreviewPrivate::MegaChatRichPreviewPrivate(const string &text, const string &title, const string &description,
                                         const string &image, const string &imageFormat, const string &icon,
                                         const string &iconFormat, const string &url)
    : mText(text), mTitle(title), mDescription(description)
    , mImage(image), mImageFormat(imageFormat), mIcon(icon)
    , mIconFormat(iconFormat), mUrl(url)
{
    mDomainName = mUrl;
    std::string::size_type position = mDomainName.find("://");
    if (position != std::string::npos)
    {
         mDomainName = mDomainName.substr(position + 3);
    }

    position = mDomainName.find("/");
    if (position != std::string::npos)
    {
        mDomainName = mDomainName.substr(0, position);
    }
}

const char *MegaChatRichPreviewPrivate::getText() const
{
    return mText.c_str();
}

const char *MegaChatRichPreviewPrivate::getTitle() const
{
    return mTitle.c_str();
}

const char *MegaChatRichPreviewPrivate::getDescription() const
{
    return mDescription.c_str();
}

const char *MegaChatRichPreviewPrivate::getImage() const
{
    return mImage.size() ? mImage.c_str() : NULL;
}

const char *MegaChatRichPreviewPrivate::getImageFormat() const
{
    return mImageFormat.c_str();
}

const char *MegaChatRichPreviewPrivate::getIcon() const
{
    return mIcon.size() ? mIcon.c_str() : NULL;
}

const char *MegaChatRichPreviewPrivate::getIconFormat() const
{
    return mIconFormat.c_str();
}

const char *MegaChatRichPreviewPrivate::getUrl() const
{
    return mUrl.c_str();
}

MegaChatRichPreview *MegaChatRichPreviewPrivate::copy() const
{
    return new MegaChatRichPreviewPrivate(this);
}

MegaChatRichPreviewPrivate::~MegaChatRichPreviewPrivate()
{

}

std::string JSonUtils::generateAttachNodeJSon(MegaNodeList *nodes, uint8_t type)
{
    std::string ret;
    if (!nodes || type == Message::kMsgInvalid)
    {
        return ret;
    }

    rapidjson::Document jSonAttachmentNodes(rapidjson::kArrayType);
    for (int i = 0; i < nodes->size(); ++i)
    {
        rapidjson::Value jsonNode(rapidjson::kObjectType);

        MegaNode *megaNode = nodes->get(i);
        if (megaNode == NULL)
        {
            API_LOG_ERROR("Invalid node at index %d", i);
            return ret;
        }

        // h -> handle
        char *base64Handle = MegaApi::handleToBase64(megaNode->getHandle());
        std::string handleString(base64Handle);
        delete [] base64Handle;
        rapidjson::Value nodeHandleValue(rapidjson::kStringType);
        nodeHandleValue.SetString(handleString.c_str(), handleString.length(), jSonAttachmentNodes.GetAllocator());
        jsonNode.AddMember(rapidjson::Value("h"), nodeHandleValue, jSonAttachmentNodes.GetAllocator());

        // k -> binary key
        char tempKey[FILENODEKEYLENGTH];
        char *base64Key = megaNode->getBase64Key();
        Base64::atob(base64Key, (::mega::byte*)tempKey, FILENODEKEYLENGTH);
        delete [] base64Key;

        // This call must be done with type <T> = <int32_t>
        std::vector<int32_t> keyVector = ::mega::Utils::str_to_a32<int32_t>(std::string(tempKey, FILENODEKEYLENGTH));
        rapidjson::Value keyVectorNode(rapidjson::kArrayType);
        if (keyVector.size() != 8)
        {
            API_LOG_ERROR("Invalid nodekey for attached node: %d", megaNode->getHandle());
            return ret;
        }
        for (unsigned int j = 0; j < keyVector.size(); ++j)
        {
            keyVectorNode.PushBack(rapidjson::Value(keyVector[j]), jSonAttachmentNodes.GetAllocator());
        }

        jsonNode.AddMember(rapidjson::Value("k"), keyVectorNode, jSonAttachmentNodes.GetAllocator());

        // t -> type
        jsonNode.AddMember(rapidjson::Value("t"), rapidjson::Value(megaNode->getType()), jSonAttachmentNodes.GetAllocator());

        // name -> name
        std::string nameString = std::string(megaNode->getName());
        rapidjson::Value nameValue(rapidjson::kStringType);
        nameValue.SetString(nameString.c_str(), nameString.length(), jSonAttachmentNodes.GetAllocator());
        jsonNode.AddMember(rapidjson::Value("name"), nameValue, jSonAttachmentNodes.GetAllocator());

        // s -> size
        jsonNode.AddMember(rapidjson::Value("s"), rapidjson::Value(megaNode->getSize()), jSonAttachmentNodes.GetAllocator());

        // hash -> fingerprint
        const char *fingerprintMega = megaNode->getFingerprint();
        char *fingerprint = NULL;
        if (fingerprintMega)
        {
            fingerprint = MegaApiImpl::getMegaFingerprintFromSdkFingerprint(fingerprintMega);
        }

        if (fingerprint)
        {
            rapidjson::Value fpValue(rapidjson::kStringType);
            fpValue.SetString(fingerprint, strlen(fingerprint), jSonAttachmentNodes.GetAllocator());
            jsonNode.AddMember(rapidjson::Value("hash"), fpValue, jSonAttachmentNodes.GetAllocator());
            delete [] fingerprint;
        }

        // fa -> image thumbnail/preview/mediainfo
        const char *fa = megaNode->getFileAttrString();
        if (fa)
        {
            std::string faString(fa);
            delete [] fa;

            rapidjson::Value faValue(rapidjson::kStringType);
            faValue.SetString(faString.c_str(), faString.length(), jSonAttachmentNodes.GetAllocator());
            jsonNode.AddMember(rapidjson::Value("fa"), faValue, jSonAttachmentNodes.GetAllocator());
        }
        else
        {
            // ar -> empty
            rapidjson::Value arValue(rapidjson::kObjectType);
            jsonNode.AddMember(rapidjson::Value("ar"), arValue, jSonAttachmentNodes.GetAllocator());
        }

        // ts -> time stamp
        jsonNode.AddMember(rapidjson::Value("ts"), rapidjson::Value(megaNode->getModificationTime()), jSonAttachmentNodes.GetAllocator());

        jSonAttachmentNodes.PushBack(jsonNode, jSonAttachmentNodes.GetAllocator());
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    jSonAttachmentNodes.Accept(writer);

    ret.assign(buffer.GetString(), buffer.GetSize());
    ret.insert(ret.begin(), type - Message::kMsgOffset);
    ret.insert(ret.begin(), 0x0);

    return ret;
}

MegaNodeList *JSonUtils::parseAttachNodeJSon(const char *json)
{
    if (!json || strcmp(json, "") == 0)
    {
        API_LOG_ERROR("Invalid attachment JSON");
        return NULL;
    }

    rapidjson::StringStream stringStream(json);
    rapidjson::Document document;
    document.ParseStream(stringStream);

    if (document.GetParseError() != rapidjson::ParseErrorCode::kParseErrorNone)
    {
        API_LOG_ERROR("parseAttachNodeJSon: Parser json error");
        return NULL;
    }

    MegaNodeList *megaNodeList = new MegaNodeListPrivate();

    int attachmentNumber = document.Capacity();
    for (int i = 0; i < attachmentNumber; ++i)
    {
        const rapidjson::Value& file = document[i];

        // nodehandle
        rapidjson::Value::ConstMemberIterator iteratorHandle = file.FindMember("h");
        if (iteratorHandle == file.MemberEnd() || !iteratorHandle->value.IsString())
        {
            API_LOG_ERROR("parseAttachNodeJSon: Invalid nodehandle in attachment JSON");
            delete megaNodeList;
            return NULL;
        }
        MegaHandle megaHandle = MegaApi::base64ToHandle(iteratorHandle->value.GetString());

        // filename
        rapidjson::Value::ConstMemberIterator iteratorName = file.FindMember("name");
        if (iteratorName == file.MemberEnd() || !iteratorName->value.IsString())
        {
            API_LOG_ERROR("parseAttachNodeJSon: Invalid filename in attachment JSON");
            delete megaNodeList;
            return NULL;
        }
        std::string nameString = iteratorName->value.GetString();

        // nodekey
        rapidjson::Value::ConstMemberIterator iteratorKey = file.FindMember("k");
        if (!iteratorKey->value.IsArray())
        {
            iteratorKey = file.FindMember("key");
        }
        if (iteratorKey == file.MemberEnd() || !iteratorKey->value.IsArray()
                || iteratorKey->value.Capacity() != 8)
        {
            API_LOG_ERROR("parseAttachNodeJSon: Invalid nodekey in attachment JSON");
            delete megaNodeList;
            return NULL;
        }
        std::vector<int32_t> kElements;
        for (unsigned int j = 0; j < iteratorKey->value.Capacity(); ++j)
        {
            if (iteratorKey->value[j].IsInt())
            {
                int32_t value = iteratorKey->value[j].GetInt();
                kElements.push_back(value);
            }
            else
            {
                API_LOG_ERROR("parseAttachNodeJSon: Invalid nodekey data in attachment JSON");
                delete megaNodeList;
                return NULL;
            }
        }

        // This call must be done with type <T> = <int32_t>
        std::string key = ::mega::Utils::a32_to_str<int32_t>(kElements);

        // size
        rapidjson::Value::ConstMemberIterator iteratorSize = file.FindMember("s");
        if (iteratorSize == file.MemberEnd() || !iteratorSize->value.IsInt64())
        {
            API_LOG_ERROR("parseAttachNodeJSon: Invalid size in attachment JSON");
            delete megaNodeList;
            return NULL;
        }
        int64_t size = iteratorSize->value.GetInt64();

        // fingerprint
        rapidjson::Value::ConstMemberIterator iteratorFp = file.FindMember("hash");
        std::string fp;
        if (iteratorFp == file.MemberEnd() || !iteratorFp->value.IsString())
        {
            API_LOG_WARNING("parseAttachNodeJSon: Missing fingerprint in attachment JSON. Old message?");
        }
        else
        {
            fp = iteratorFp->value.GetString();
        }
        // convert MEGA's fingerprint to the internal format used by SDK (includes size)
        char *sdkFingerprint = !fp.empty() ? MegaApiImpl::getSdkFingerprintFromMegaFingerprint(fp.c_str(), size) : NULL;

        // nodetype
        rapidjson::Value::ConstMemberIterator iteratorType = file.FindMember("t");
        if (iteratorType == file.MemberEnd() || !iteratorType->value.IsInt())
        {
            API_LOG_ERROR("parseAttachNodeJSon: Invalid type in attachment JSON");
            delete megaNodeList;
            return NULL;
        }
        int type = iteratorType->value.GetInt();

        // timestamp
        rapidjson::Value::ConstMemberIterator iteratorTimeStamp = file.FindMember("ts");
        if (iteratorTimeStamp == file.MemberEnd() || !iteratorTimeStamp->value.IsInt64())
        {
            API_LOG_ERROR("parseAttachNodeJSon: Invalid timestamp in attachment JSON");
            delete megaNodeList;
            return NULL;
        }
        int64_t timeStamp = iteratorTimeStamp->value.GetInt64();

        // file-attrstring
        rapidjson::Value::ConstMemberIterator iteratorFa = file.FindMember("fa");
        std::string fa;
        if (iteratorFa != file.MemberEnd() && iteratorFa->value.IsString())
        {
            fa = iteratorFa->value.GetString();
        }

        std::string attrstring;
        MegaNodePrivate node(nameString.c_str(), type, size, timeStamp, timeStamp,
                             megaHandle, &key, &attrstring, &fa, sdkFingerprint,
                             NULL, INVALID_HANDLE, INVALID_HANDLE, NULL, NULL, false, true);

        megaNodeList->addNode(&node);

        delete [] sdkFingerprint;
    }

    return megaNodeList;
}

std::string JSonUtils::generateAttachContactJSon(MegaHandleList *contacts, ContactList *contactList)
{
    std::string ret;
    if (!contacts || contacts->size() == 0 || !contactList || contacts->size() > contactList->size())
    {
        API_LOG_ERROR("parseAttachContactJSon: no contacts available");
        return ret;
    }

    rapidjson::Document jSonDocument(rapidjson::kArrayType);
    for (unsigned int i = 0; i < contacts->size(); ++i)
    {
        auto contactIterator = contactList->find(contacts->get(i));
        if (contactIterator != contactList->end())
        {
            karere::Contact* contact = contactIterator->second;

            rapidjson::Value jSonContact(rapidjson::kObjectType);
            const char *base64Handle = MegaApi::userHandleToBase64(contact->userId());
            std::string handleString(base64Handle);
            rapidjson::Value userHandleValue(rapidjson::kStringType);
            userHandleValue.SetString(handleString.c_str(), handleString.length(), jSonDocument.GetAllocator());
            jSonContact.AddMember(rapidjson::Value("u"), userHandleValue, jSonDocument.GetAllocator());
            delete [] base64Handle;

            rapidjson::Value emailValue(rapidjson::kStringType);
            emailValue.SetString(contact->email().c_str(), contact->email().length(), jSonDocument.GetAllocator());
            jSonContact.AddMember(rapidjson::Value("email"), emailValue, jSonDocument.GetAllocator());

            std::string nameString = contact->getContactName();
            rapidjson::Value nameValue(rapidjson::kStringType);
            nameValue.SetString(nameString.c_str(), nameString.length(), jSonDocument.GetAllocator());
            jSonContact.AddMember(rapidjson::Value("name"), nameValue, jSonDocument.GetAllocator());

            jSonDocument.PushBack(jSonContact, jSonDocument.GetAllocator());
        }
        else
        {
            API_LOG_ERROR("Failed to find the contact: %d", contacts->get(i));
            return ret;
        }
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    jSonDocument.Accept(writer);

    // assemble final message with the type
    ret.assign(buffer.GetString(), buffer.GetSize());
    ret.insert(ret.begin(), Message::kMsgContact - Message::kMsgOffset);
    ret.insert(ret.begin(), 0x0);

    return ret;
}

std::vector<MegaChatAttachedUser> *JSonUtils::parseAttachContactJSon(const char *json)
{
    if (!json  || strcmp(json, "") == 0)
    {
        return NULL;
    }

    rapidjson::StringStream stringStream(json);

    rapidjson::Document document;
    document.ParseStream(stringStream);

    if (document.GetParseError() != rapidjson::ParseErrorCode::kParseErrorNone)
    {
        API_LOG_ERROR("parseAttachContactJSon: Parser json error");
        return NULL;
    }

    std::vector<MegaChatAttachedUser> *megaChatUsers = new std::vector<MegaChatAttachedUser>();

    int contactNumber = document.Capacity();
    for (int i = 0; i < contactNumber; ++i)
    {
        const rapidjson::Value& user = document[i];

        rapidjson::Value::ConstMemberIterator iteratorEmail = user.FindMember("email");
        if (iteratorEmail == user.MemberEnd() || !iteratorEmail->value.IsString())
        {
            API_LOG_ERROR("parseAttachContactJSon: Invalid email in contact-attachment JSON");
            delete megaChatUsers;
            return NULL;
        }
        std::string emailString = iteratorEmail->value.GetString();

        rapidjson::Value::ConstMemberIterator iteratorHandle = user.FindMember("u");
        if (iteratorHandle == user.MemberEnd() || !iteratorHandle->value.IsString())
        {
            API_LOG_ERROR("parseAttachContactJSon: Invalid userhandle in contact-attachment JSON");
            delete megaChatUsers;
            return NULL;
        }
        std::string handleString = iteratorHandle->value.GetString();

        rapidjson::Value::ConstMemberIterator iteratorName = user.FindMember("name");
        if (iteratorName == user.MemberEnd() || !iteratorName->value.IsString())
        {
            API_LOG_ERROR("parseAttachContactJSon: Invalid username in contact-attachment JSON");
            delete megaChatUsers;
            return NULL;
        }
        std::string nameString = iteratorName->value.GetString();

        MegaChatAttachedUser megaChatUser(MegaApi::base64ToUserHandle(handleString.c_str()) , emailString, nameString);
        megaChatUsers->push_back(megaChatUser);
    }

    return megaChatUsers;

}

std::string JSonUtils::generateGeolocationJSon(float longitude, float latitude, const char *img)
{
    std::string textMessage("https://www.google.com/maps/search/?api=1&query=");
    textMessage.append(std::to_string(latitude)).append(",").append(std::to_string(longitude));

    // Add generic `textMessage`
    rapidjson::Document jsonContainsMeta(rapidjson::kObjectType);
    rapidjson::Value jsonTextMessage(rapidjson::kStringType);
    jsonTextMessage.SetString(textMessage.c_str(), textMessage.length(), jsonContainsMeta.GetAllocator());
    jsonContainsMeta.AddMember(rapidjson::Value("textMessage"), jsonTextMessage, jsonContainsMeta.GetAllocator());

    // prepare geolocation object: longitude, latitude, image
    rapidjson::Value jsonGeolocation(rapidjson::kObjectType);
    // longitud
    rapidjson::Value jsonLongitude(rapidjson::kStringType);
    std::string longitudeString = std::to_string(longitude);
    jsonLongitude.SetString(longitudeString.c_str(), longitudeString.length());
    jsonGeolocation.AddMember(rapidjson::Value("lng"), jsonLongitude, jsonContainsMeta.GetAllocator());
    // latitude
    rapidjson::Value jsonLatitude(rapidjson::kStringType);
    std::string latitudeString = std::to_string(latitude);
    jsonLatitude.SetString(latitudeString.c_str(), latitudeString.length());
    jsonGeolocation.AddMember(rapidjson::Value("la"), jsonLatitude, jsonContainsMeta.GetAllocator());
    // image/thumbnail
    if (img)
    {
        rapidjson::Value jsonImage(rapidjson::kStringType);
        jsonImage.SetString(img, strlen(img), jsonContainsMeta.GetAllocator());
        jsonGeolocation.AddMember(rapidjson::Value("img"), jsonImage, jsonContainsMeta.GetAllocator());
    }

    // Add the `extra` with the geolocation data
    rapidjson::Value jsonExtra(rapidjson::kArrayType);
    jsonExtra.PushBack(jsonGeolocation, jsonContainsMeta.GetAllocator());
    jsonContainsMeta.AddMember(rapidjson::Value("extra"), jsonExtra, jsonContainsMeta.GetAllocator());

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    jsonContainsMeta.Accept(writer);

    // assemble final message with the type (contains-meta) and subtype (geolocation)
    std::string message(buffer.GetString(), buffer.GetSize());
    message.insert(message.begin(), Message::ContainsMetaSubType::kGeoLocation);
    message.insert(message.begin(), Message::kMsgContainsMeta - Message::kMsgOffset);
    message.insert(message.begin(), 0x0);

    return message;
}

std::string JSonUtils::generateGiphyJSon(const char* srcMp4, const char* srcWebp, long long sizeMp4, long long sizeWebp, int width, int height, const char* title)
{
    std::string ret;
    if (!srcMp4 || sizeMp4 == 0 || !srcWebp || sizeWebp == 0 || !title)
    {
        API_LOG_ERROR("generateGiphyJSon: Insufficient information");
        return ret;
    }

    rapidjson::Document jsonContainsMeta(rapidjson::kObjectType);

    // Add generic `textMessage`
    rapidjson::Value jsonTextMessage(rapidjson::kStringType);
    std::string textMessage(title);
    jsonTextMessage.SetString(textMessage.c_str(), textMessage.length(), jsonContainsMeta.GetAllocator());
    jsonContainsMeta.AddMember(rapidjson::Value("textMessage"), jsonTextMessage, jsonContainsMeta.GetAllocator());

    // prepare giphy object: mp4, webp, mp4 size, webp size, giphy width and giphy height
    rapidjson::Value jsonGiphy(rapidjson::kObjectType);

    // srcMp4
    rapidjson::Value jsonMp4(rapidjson::kStringType);
    std::string mp4String(srcMp4);
    jsonMp4.SetString(mp4String.c_str(), mp4String.length());
    jsonContainsMeta.AddMember(rapidjson::Value("src"), jsonMp4, jsonContainsMeta.GetAllocator());

    // srcWebp
    rapidjson::Value jsonWebp(rapidjson::kStringType);
    std::string webpString(srcWebp);
    jsonWebp.SetString(webpString.c_str(), webpString.length());
    jsonContainsMeta.AddMember(rapidjson::Value("src_webp"), jsonWebp, jsonContainsMeta.GetAllocator());

    // mp4 size
    rapidjson::Value jsonMp4Size(rapidjson::kStringType);
    std::string mp4sizeString = std::to_string(sizeMp4);
    jsonMp4Size.SetString(mp4sizeString.c_str(), mp4sizeString.length());
    jsonContainsMeta.AddMember(rapidjson::Value("s"), jsonMp4Size, jsonContainsMeta.GetAllocator());

    // webp size
    rapidjson::Value jsonWebpSize(rapidjson::kStringType);
    std::string webpsizeString = std::to_string(sizeWebp);
    jsonWebpSize.SetString(webpsizeString.c_str(), webpsizeString.length());
    jsonContainsMeta.AddMember(rapidjson::Value("s_webp"), jsonWebpSize, jsonContainsMeta.GetAllocator());

    // width
    rapidjson::Value jsonGiphyWidth(rapidjson::kStringType);
    std::string giphyWidthString = std::to_string(width);
    jsonGiphyWidth.SetString(giphyWidthString.c_str(), giphyWidthString.length());
    jsonContainsMeta.AddMember(rapidjson::Value("w"), jsonGiphyWidth, jsonContainsMeta.GetAllocator());

    // height
    rapidjson::Value jsonGiphyHeight(rapidjson::kStringType);
    std::string giphyHeightString = std::to_string(height);
    jsonGiphyHeight.SetString(giphyHeightString.c_str(), giphyHeightString.length());
    jsonContainsMeta.AddMember(rapidjson::Value("h"), jsonGiphyHeight, jsonContainsMeta.GetAllocator());

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    jsonContainsMeta.Accept(writer);

    // assemble final message with the type (contains-meta) and subtype (giphy)
    std::string message(buffer.GetString(), buffer.GetSize());
    message.insert(message.begin(), Message::ContainsMetaSubType::kGiphy);
    message.insert(message.begin(), Message::kMsgContainsMeta - Message::kMsgOffset);
    message.insert(message.begin(), 0x0);

    return message;
}

string JSonUtils::getLastMessageContent(const string& content, uint8_t type)
{
    std::string messageContents;
    switch (type)
    {
        case MegaChatMessage::TYPE_CONTACT_ATTACHMENT:
        {
            // Remove the first two characters. [0] = 0x0 | [1] = Message::kMsgContact
            std::string messageAttach = content;
            messageAttach.erase(messageAttach.begin(), messageAttach.begin() + 2);

            std::vector<MegaChatAttachedUser> *userVector = JSonUtils::parseAttachContactJSon(messageAttach.c_str());
            if (userVector && userVector->size() > 0)
            {
                for (unsigned int i = 0; i < userVector->size() - 1; ++i)
                {
                    messageContents.append(userVector->at(i).getName());
                    // We use character 0x01 as separator
                    messageContents.push_back(0x01);
                }

                messageContents.append(userVector->at(userVector->size() - 1).getName());
            }

            delete userVector;
            break;
        }
        case MegaChatMessage::TYPE_VOICE_CLIP:  // fall-through
        case MegaChatMessage::TYPE_NODE_ATTACHMENT:
        {
            // Remove the first two characters. [0] = 0x0 | [1] = Message::kMsgAttachment/kMsgVoiceClip
            std::string messageAttach = content;
            messageAttach.erase(messageAttach.begin(), messageAttach.begin() + 2);

            MegaNodeList *megaNodeList = JSonUtils::parseAttachNodeJSon(messageAttach.c_str());
            if (megaNodeList && megaNodeList->size() > 0)
            {
                for (int i = 0; i < megaNodeList->size() - 1; ++i)
                {
                    messageContents.append(megaNodeList->get(i)->getName());
                    // We use character 0x01 as separator
                    messageContents.push_back(0x01);
                }

                messageContents.append(megaNodeList->get(megaNodeList->size() - 1)->getName());
            }

            delete megaNodeList;
            break;
        }
        case MegaChatMessage::TYPE_CONTAINS_META:
        {
            if (content.size() > 4)
            {
                // Remove the first three characters. [0] = 0x0 | [1] = Message::kMsgContaintsMeta | [2] = subtype
                uint8_t containsMetaType = content.at(2);
                const char *containsMetaJson = content.data() + 3;
                const MegaChatContainsMeta *containsMeta = JSonUtils::parseContainsMeta(containsMetaJson, containsMetaType, true);
                messageContents = containsMeta->getTextMessage();
                delete containsMeta;
            }
            break;
        }
        default:
        {
            messageContents = content;
            break;
        }
    }

    return messageContents;
}

const MegaChatContainsMeta* JSonUtils::parseContainsMeta(const char *json, uint8_t type, bool onlyTextMessage)
{
    MegaChatContainsMetaPrivate *containsMeta = new MegaChatContainsMetaPrivate();
    if (!json || !strlen(json))
    {
        API_LOG_ERROR("parseContainsMeta: invalid JSON struct - JSON contains no data, only includes type of meta");
        return containsMeta;
    }

    rapidjson::StringStream stringStream(json);

    rapidjson::Document document;
    document.ParseStream(stringStream);
    if (document.GetParseError() != rapidjson::ParseErrorCode::kParseErrorNone)
    {
        API_LOG_ERROR("parseContainsMeta: Parser JSON error");
        return containsMeta;
    }

    rapidjson::Value::ConstMemberIterator iteratorTextMessage = document.FindMember("textMessage");
    if (iteratorTextMessage == document.MemberEnd() || !iteratorTextMessage->value.IsString())
    {
        API_LOG_ERROR("parseContainsMeta: invalid JSON struct - \"textMessage\" field not found");
        return containsMeta;
    }
    std::string textMessage = iteratorTextMessage->value.GetString();
    containsMeta->setTextMessage(textMessage);

    if (!onlyTextMessage)
    {
        switch (type)
        {
            case MegaChatContainsMeta::CONTAINS_META_RICH_PREVIEW:
            {
                MegaChatRichPreview *richPreview = parseRichPreview(document, textMessage);
                containsMeta->setRichPreview(richPreview);
                break;
            }
            case MegaChatContainsMeta::CONTAINS_META_GEOLOCATION:
            {
                MegaChatGeolocation *geolocation = parseGeolocation(document);
                containsMeta->setGeolocation(geolocation);
                break;
            }
            case MegaChatContainsMeta::CONTAINS_META_GIPHY:
            {
                auto giphy = parseGiphy(document);
                containsMeta->setGiphy(move(giphy));
                break;
            }
            default:
            {
                API_LOG_ERROR("parseContainsMeta: unknown type of message with meta contained");
                break;
            }
        }
    }

    return containsMeta;
}

MegaChatRichPreview *JSonUtils::parseRichPreview(rapidjson::Document &document, std::string &textMessage)
{
    rapidjson::Value::ConstMemberIterator iteratorExtra = document.FindMember("extra");
    if (iteratorExtra == document.MemberEnd() || iteratorExtra->value.IsObject())
    {
        API_LOG_ERROR("parseRichPreview: invalid JSON struct - \"extra\" field not found");
        return NULL;
    }

    std::string title;
    std::string description;
    std::string image;
    std::string imageFormat;
    std::string icon;
    std::string iconFormat;
    std::string url;

    if (iteratorExtra->value.Capacity() == 1)
    {
        const rapidjson::Value& richPreview = iteratorExtra->value[0];

        rapidjson::Value::ConstMemberIterator iteratorTitle = richPreview.FindMember("t");
        if (iteratorTitle != richPreview.MemberEnd() && iteratorTitle->value.IsString())
        {
            title = iteratorTitle->value.GetString();
        }

        rapidjson::Value::ConstMemberIterator iteratorDescription = richPreview.FindMember("d");
        if (iteratorDescription != richPreview.MemberEnd() && iteratorDescription->value.IsString())
        {
            description = iteratorDescription->value.GetString();
        }

        getRichLinckImageFromJson("i", richPreview, image, imageFormat);
        getRichLinckImageFromJson("ic", richPreview, icon, iconFormat);

        rapidjson::Value::ConstMemberIterator iteratorURL = richPreview.FindMember("url");
        if (iteratorURL != richPreview.MemberEnd() && iteratorURL->value.IsString())
        {
            url = iteratorURL->value.GetString();
        }
    }

    return new MegaChatRichPreviewPrivate(textMessage, title, description, image, imageFormat, icon, iconFormat, url);
}

MegaChatGeolocation *JSonUtils::parseGeolocation(rapidjson::Document &document)
{
    rapidjson::Value::ConstMemberIterator iteratorExtra = document.FindMember("extra");
    if (iteratorExtra == document.MemberEnd() || iteratorExtra->value.IsObject())
    {
        API_LOG_ERROR("parseGeolocation: invalid JSON struct - \"extra\" field not found");
        return NULL;
    }

    if (iteratorExtra->value.Capacity() != 1)
    {
        API_LOG_ERROR("parseGeolocation: invalid JSON struct - invalid format");
        return NULL;
    }

    float longitude;
    float latitude;
    std::string image;

    const rapidjson::Value &geolocationValue = iteratorExtra->value[0];

    rapidjson::Value::ConstMemberIterator iteratorLongitude = geolocationValue.FindMember("lng");
    if (iteratorLongitude != geolocationValue.MemberEnd() && iteratorLongitude->value.IsString())
    {
        const char *longitudeString = iteratorLongitude->value.GetString();
        longitude = atof(longitudeString);
    }
    else
    {
        API_LOG_ERROR("parseGeolocation: invalid JSON struct - \"lng\" not found");
        return NULL;
    }

    rapidjson::Value::ConstMemberIterator iteratorLatitude = geolocationValue.FindMember("la");
    if (iteratorLatitude != geolocationValue.MemberEnd() && iteratorLatitude->value.IsString())
    {
        const char *latitudeString = iteratorLatitude->value.GetString();
        latitude = atof(latitudeString);
    }
    else
    {
        API_LOG_ERROR("parseGeolocation: invalid JSON struct - \"la\" not found");
        return NULL;
    }

    rapidjson::Value::ConstMemberIterator iteratorImage = geolocationValue.FindMember("img");
    if (iteratorImage != geolocationValue.MemberEnd() && iteratorImage->value.IsString())
    {
        const char *imagePointer = iteratorImage->value.GetString();
        size_t imageSize = iteratorImage->value.GetStringLength();
        image.assign(imagePointer, imageSize);
    }
    else
    {
        API_LOG_WARNING("parseGeolocation: invalid JSON struct - \"img\" not found");
        // image is not mandatory
    }

    return new MegaChatGeolocationPrivate(longitude, latitude, image);
}

std::unique_ptr<MegaChatGiphy> JSonUtils::parseGiphy(rapidjson::Document& document)
{
    auto textMessageIterator = document.FindMember("textMessage");
    string giphyTitle;
    if (textMessageIterator != document.MemberEnd() && textMessageIterator->value.IsString())
    {
        giphyTitle.assign(textMessageIterator->value.GetString(), textMessageIterator->value.GetStringLength());
    }
    else
    {
        API_LOG_ERROR("parseGiphy: invalid JSON struct - \"textMessage\" field not found");
        return std::unique_ptr<MegaChatGiphy>(nullptr);
    }

    auto mp4srcIterator = document.FindMember("src");
    string mp4srcString;
    if (mp4srcIterator != document.MemberEnd() && mp4srcIterator->value.IsString())
    {
        mp4srcString.assign(mp4srcIterator->value.GetString(), mp4srcIterator->value.GetStringLength());
    }
    else
    {
        API_LOG_ERROR("parseGiphy: invalid JSON struct - \"src\" field not found");
        return std::unique_ptr<MegaChatGiphy>(nullptr);
    }

    auto webpIterator = document.FindMember("src_webp");
    string webpsrcString;
    if (webpIterator != document.MemberEnd() && webpIterator->value.IsString())
    {
        webpsrcString.assign(webpIterator->value.GetString(), webpIterator->value.GetStringLength());
    }
    else
    {
        API_LOG_ERROR("parseGiphy: invalid JSON struct - \"src_webp\" field not found");
        return std::unique_ptr<MegaChatGiphy>(nullptr);
    }

    auto mp4sizeIterator = document.FindMember("s");
    long mp4Size = 0;
    if (mp4sizeIterator != document.MemberEnd() && mp4sizeIterator->value.IsString())
    {
        auto mp4sizeString = mp4sizeIterator->value.GetString();
        mp4Size = atol(mp4sizeString);
    }
    else
    {
        API_LOG_ERROR("parseGiphy: invalid JSON struct - \"s\" field not found");
        return std::unique_ptr<MegaChatGiphy>(nullptr);
    }

    auto webpsizeIterator = document.FindMember("s_webp");
    long webpSize = 0;
    if (webpsizeIterator != document.MemberEnd() && webpsizeIterator->value.IsString())
    {
        auto webpsizeString = webpsizeIterator->value.GetString();
        webpSize = atol(webpsizeString);
    }
    else
    {
        API_LOG_ERROR("parseGiphy: invalid JSON struct - \"s_webp\" field not found");
        return std::unique_ptr<MegaChatGiphy>(nullptr);
    }

    auto giphywidthIterator = document.FindMember("w");
    int giphyWidth = 0;
    if (giphywidthIterator != document.MemberEnd() && giphywidthIterator->value.IsString())
    {
        auto giphywidthString = giphywidthIterator->value.GetString();
        giphyWidth = atoi(giphywidthString);
    }
    else
    {
        API_LOG_ERROR("parseGiphy: invalid JSON struct - \"w\" field not found");
        return std::unique_ptr<MegaChatGiphy>(nullptr);
    }

    auto giphyheightIterator = document.FindMember("h");
    int giphyHeight = 0;
    if (giphyheightIterator != document.MemberEnd() && giphyheightIterator->value.IsString())
    {
        auto giphyheightString = giphyheightIterator->value.GetString();
        giphyHeight = atoi(giphyheightString);
    }
    else
    {
        API_LOG_ERROR("parseGiphy: invalid JSON struct - \"h\" field not found");
        return std::unique_ptr<MegaChatGiphy>(nullptr);
    }
    return ::mega::make_unique<MegaChatGiphyPrivate>(mp4srcString, webpsrcString, mp4Size, webpSize, giphyWidth, giphyHeight, giphyTitle);
}

string JSonUtils::getImageFormat(const char *imagen)
{
    std::string format;
    size_t size = strlen(imagen);

    size_t i = 0;
    while (imagen[i] != ':' && i < size)
    {
        format += imagen[i];
        i++;
    }

    return format;
}

void JSonUtils::getRichLinckImageFromJson(const string &field, const rapidjson::Value& richPreviewValue, string &image, string &format)
{
    rapidjson::Value::ConstMemberIterator iteratorImage = richPreviewValue.FindMember(field.c_str());
    if (iteratorImage != richPreviewValue.MemberEnd() && iteratorImage->value.IsString())
    {
        const char *imagePointer = iteratorImage->value.GetString();
        format = getImageFormat(imagePointer);
        rapidjson::SizeType sizeImage = iteratorImage->value.GetStringLength();
        if (format.size() > 10 || format.size() == sizeImage)
        {
            format = "";
            API_LOG_ERROR("Parse rich link: \"%s\" Invalid image extension", field.c_str());
            return;
        }

        imagePointer = imagePointer + format.size() + 1; // remove format.size() + ':'

        // Check if the image format in B64 is valid
        std::string imgBin, imgB64(imagePointer);
        size_t binSize = Base64::atob(imgB64, imgBin);
        size_t paddingSize = std::count(imgB64.begin(), imgB64.end(), '=');
        if (binSize == (imgB64.size() * 3) / 4 - paddingSize)
        {
            image = std::string(imagePointer, sizeImage - (format.size() + 1));
        }
        else
        {
            API_LOG_ERROR("Parse rich link: \"%s\" field has a invalid format", field.c_str());
        }
    }
    else
    {
        API_LOG_ERROR("Parse rich link: invalid JSON struct - \"%s\" field not found", field.c_str());
    }
}

const char *MegaChatRichPreviewPrivate::getDomainName() const
{
    return mDomainName.c_str();
}

MegaChatGeolocationPrivate::MegaChatGeolocationPrivate(float longitude, float latitude, const string &image)
    : mLongitude(longitude), mLatitude(latitude), mImage(image)
{
}

MegaChatGeolocationPrivate::MegaChatGeolocationPrivate(const MegaChatGeolocationPrivate *geolocation)
{
    mLongitude = geolocation->mLongitude;
    mLatitude = geolocation->mLatitude;
    mImage = geolocation->mImage;
}

MegaChatGeolocation *MegaChatGeolocationPrivate::copy() const
{
    return new MegaChatGeolocationPrivate(this);
}

float MegaChatGeolocationPrivate::getLongitude() const
{
    return mLongitude;
}

float MegaChatGeolocationPrivate::getLatitude() const
{
    return mLatitude;
}

const char *MegaChatGeolocationPrivate::getImage() const
{
    return mImage.size() ? mImage.c_str() : NULL;
}

MegaChatGiphyPrivate::MegaChatGiphyPrivate(const std::string& srcMp4, const std::string& srcWebp, long long sizeMp4, long long sizeWebp, int width, int height, const std::string& title)
    :mMp4Src(srcMp4), mWebpSrc(srcWebp), mTitle(title), mMp4Size(sizeMp4), mWebpSize(sizeWebp), mWidth(width), mHeight(height)
{
}

MegaChatGiphyPrivate::MegaChatGiphyPrivate(const MegaChatGiphyPrivate* giphy)
{
    mMp4Src = giphy->mMp4Src;
    mWebpSrc = giphy->mWebpSrc;
    mTitle = giphy->mTitle;
    mMp4Size = giphy->mMp4Size;
    mWebpSize = giphy->mWebpSize;
    mWidth = giphy->mWidth;
    mHeight = giphy->mHeight;
}

MegaChatGiphy* MegaChatGiphyPrivate::copy() const
{
    return new MegaChatGiphyPrivate(this);
}

const char* MegaChatGiphyPrivate::getMp4Src() const
{
    return mMp4Src.c_str();
}

const char* MegaChatGiphyPrivate::getWebpSrc() const
{
    return mWebpSrc.c_str();
}

int MegaChatGiphyPrivate::getWidth() const
{
    return mWidth;
}

int MegaChatGiphyPrivate::getHeight() const
{
    return mHeight;
}

const char* MegaChatGiphyPrivate::getTitle() const
{
    return mTitle.size() ? mTitle.c_str() : nullptr;
}

long MegaChatGiphyPrivate::getMp4Size() const
{
    return mMp4Size;
}

long MegaChatGiphyPrivate::getWebpSize() const
{
    return mWebpSize;
}

MegaChatNodeHistoryHandler::MegaChatNodeHistoryHandler(MegaChatApi *api)
    : chatApi(api)
{
}

void MegaChatNodeHistoryHandler::fireOnAttachmentReceived(MegaChatMessage *message)
{
    for(set<MegaChatNodeHistoryListener *>::iterator it = nodeHistoryListeners.begin(); it != nodeHistoryListeners.end() ; it++)
    {
        (*it)->onAttachmentReceived(chatApi, message);
    }

    delete message;
}

void MegaChatNodeHistoryHandler::fireOnAttachmentLoaded(MegaChatMessage *message)
{
    for(set<MegaChatNodeHistoryListener *>::iterator it = nodeHistoryListeners.begin(); it != nodeHistoryListeners.end() ; it++)
    {
        (*it)->onAttachmentLoaded(chatApi, message);
    }

    delete message;
}

void MegaChatNodeHistoryHandler::fireOnAttachmentDeleted(Id id)
{
    for(set<MegaChatNodeHistoryListener *>::iterator it = nodeHistoryListeners.begin(); it != nodeHistoryListeners.end() ; it++)
    {
        (*it)->onAttachmentDeleted(chatApi, id);
    }
}

void MegaChatNodeHistoryHandler::fireOnTruncate(Id id)
{
    for(set<MegaChatNodeHistoryListener *>::iterator it = nodeHistoryListeners.begin(); it != nodeHistoryListeners.end() ; it++)
    {
        (*it)->onTruncate(chatApi, id);
    }
}

void MegaChatNodeHistoryHandler::onReceived(Message *msg, Idx idx)
{
    MegaChatMessagePrivate *message = new MegaChatMessagePrivate(*msg, Message::Status::kServerReceived, idx);
    fireOnAttachmentReceived(message);
}

void MegaChatNodeHistoryHandler::onLoaded(Message *msg, Idx idx)
{
    MegaChatMessagePrivate *message = NULL;
    if (msg)
    {
        if (msg->isDeleted())
        {
            return;
        }
        message = new MegaChatMessagePrivate(*msg, Message::Status::kServerReceived, idx);
    }

    fireOnAttachmentLoaded(message);
}

void MegaChatNodeHistoryHandler::onDeleted(Id id)
{
    fireOnAttachmentDeleted(id);
}

void MegaChatNodeHistoryHandler::onTruncated(Id id)
{
    fireOnTruncate(id);
}


void MegaChatNodeHistoryHandler::addMegaNodeHistoryListener(MegaChatNodeHistoryListener *listener)
{
    nodeHistoryListeners.insert(listener);
}

void MegaChatNodeHistoryHandler::removeMegaNodeHistoryListener(MegaChatNodeHistoryListener *listener)
{
    nodeHistoryListeners.insert(listener);
}
