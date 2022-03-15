#include "QTMegaChatRoomListener.h"
#include "QTMegaChatEvent.h"

#include <QCoreApplication>

using namespace megachat;

QTMegaChatRoomListener::QTMegaChatRoomListener(MegaChatApi *megaChatApi, MegaChatRoomListener *listener) : QObject()
{
    this->megaChatApi = megaChatApi;
    this->listener = listener;
}

QTMegaChatRoomListener::~QTMegaChatRoomListener()
{ }

void QTMegaChatRoomListener::onChatRoomUpdate(MegaChatApi *api, MegaChatRoom *chat)
{
    QTMegaChatEvent *event = new QTMegaChatEvent(api, (QEvent::Type)QTMegaChatEvent::OnChatRoomUpdate);
    event->setChatRoom(chat->copy());
    QCoreApplication::postEvent(this, event, INT_MIN);
}

void QTMegaChatRoomListener::onMessageLoaded(MegaChatApi *api, MegaChatMessage *msg)
{
    QTMegaChatEvent *event = new QTMegaChatEvent(api, (QEvent::Type)QTMegaChatEvent::OnMessageLoaded);
    if (msg)
    {
        event->setChatMessage(msg->copy());
    }
    else
    {
        event->setChatMessage(NULL);
    }
    QCoreApplication::postEvent(this, event, INT_MIN);
}

void QTMegaChatRoomListener::onHistoryTruncatedByRetentionTime(MegaChatApi *api, MegaChatMessage *msg)
{
    QTMegaChatEvent *event = new QTMegaChatEvent(api, (QEvent::Type)QTMegaChatEvent::OnHistoryTruncatedByRetentionTime);
    event->setChatMessage(msg->copy());
    QCoreApplication::postEvent(this, event, INT_MIN);
}

void QTMegaChatRoomListener::onMessageReceived(MegaChatApi *api, MegaChatMessage *msg)
{
    QTMegaChatEvent *event = new QTMegaChatEvent(api, (QEvent::Type)QTMegaChatEvent::OnMessageReceived);
    event->setChatMessage(msg->copy());
    QCoreApplication::postEvent(this, event, INT_MIN);
}

void QTMegaChatRoomListener::onReactionUpdate(MegaChatApi *api, MegaChatHandle msgid, const char *reaction, int count)
{
    QTMegaChatEvent *event = new QTMegaChatEvent(api, (QEvent::Type)QTMegaChatEvent::OnReactionUpdated);
    event->setChatHandle(msgid);
    event->setWidth(count);
    event->setBuffer(::mega::MegaApi::strdup(reaction));
    QCoreApplication::postEvent(this, event, INT_MIN);
}

void QTMegaChatRoomListener::onMessageUpdate(MegaChatApi *api, MegaChatMessage *msg)
{
    QTMegaChatEvent *event = new QTMegaChatEvent(api, (QEvent::Type)QTMegaChatEvent::OnMessageUpdate);
    event->setChatMessage(msg->copy());
    QCoreApplication::postEvent(this, event, INT_MIN);
}

void QTMegaChatRoomListener::onHistoryReloaded(MegaChatApi *api, MegaChatRoom *chat)
{
    QTMegaChatEvent *event = new QTMegaChatEvent(api, (QEvent::Type)QTMegaChatEvent::OnHistoryReloaded);
    event->setChatRoom(chat->copy());
    QCoreApplication::postEvent(this, event, INT_MIN);
}

void QTMegaChatRoomListener::customEvent(QEvent *e)
{
    QTMegaChatEvent *event = (QTMegaChatEvent *)e;
    switch(static_cast<QTMegaChatEvent::MegaType>(event->type()))
    {
        case QTMegaChatEvent::OnChatRoomUpdate:
            if (listener) listener->onChatRoomUpdate(event->getMegaChatApi(), event->getChatRoom());
            break;
        case QTMegaChatEvent::OnMessageLoaded:
            if (listener) listener->onMessageLoaded(event->getMegaChatApi(), event->getChatMessage());
            break;
        case QTMegaChatEvent::OnMessageReceived:
            if (listener) listener->onMessageReceived(event->getMegaChatApi(), event->getChatMessage());
            break;
        case QTMegaChatEvent::OnMessageUpdate:
            if (listener) listener->onMessageUpdate(event->getMegaChatApi(), event->getChatMessage());
            break;
        case QTMegaChatEvent::OnHistoryReloaded:
            if (listener) listener->onHistoryReloaded(event->getMegaChatApi(), event->getChatRoom());
            break;
        case QTMegaChatEvent::OnReactionUpdated:
            if (listener) listener->onReactionUpdate(event->getMegaChatApi(), event->getChatHandle(), event->getBuffer(), event->getWidth());
            break;
        case QTMegaChatEvent::OnHistoryTruncatedByRetentionTime:
            if (listener) listener->onHistoryTruncatedByRetentionTime(event->getMegaChatApi(), event->getChatMessage());
            break;
        default:
            break;
    }
}
