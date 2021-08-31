#include "contactItemWidget.h"
#include "ui_listItemWidget.h"
#include "uiSettings.h"
#include <QMessageBox>
#include <QMenu>
#include <QClipboard>

using namespace megachat;

ContactItemWidget::ContactItemWidget(QWidget *parent, MainWindow *mainWin, megachat::MegaChatApi *megaChatApi, ::mega::MegaApi *megaApi, ::mega::MegaUser *contact) :
    QWidget(parent),
    ui(new Ui::ChatItem),
    mController(mainWin->getContactControllerById(contact->getHandle()))
{
    mMainWin = mainWin;
    mMegaApi = megaApi;
    mMegaChatApi = megaChatApi;
    mUserHandle = contact->getHandle();
    mUserVisibility = contact->getVisibility();
    const char *contactEmail = contact->getEmail();
    ui->setupUi(this);
    setAvatarStyle();
    ui->mUnreadIndicator->hide();
    ui->mPreviewersIndicator->hide();
    QString text = QString::fromUtf8(contactEmail);
    ui->mName->setText(text);

    const char *firstname = mMainWin->mApp->getFirstname(contact->getHandle(), NULL);
    mName = firstname ? std::string(firstname) : std::string();
    delete [] firstname;
    mAlias = mMainWin->mApp->getLocalUserAlias(mUserHandle);
    updateTitle();

    int status = mMegaChatApi->getUserOnlineStatus(mUserHandle);
    updateOnlineIndicator(status);
}

void ContactItemWidget::setAvatarStyle()
{
    QColor & col = gAvatarColors[mUserHandle & 0x0f];
    QString style = "border-radius: 4px;"
            "border: 2px solid rgba(0,0,0,0);"
            "color: white;"
            "font: 24px;"
            "background-color: qlineargradient(spread:pad, x1:0, y1:0, x2:1, y2:0,"
            "stop:0 rgba(%1,%2,%3,180), stop:1 rgba(%1,%2,%3,255))";
    style = style.arg(col.red()).arg(col.green()).arg(col.blue());
    ui->mAvatar->setStyleSheet(style);
}

void ContactItemWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);

    QMenu *chatMenu = menu.addMenu("Chats");

    auto chatPeerInviteAction = chatMenu->addAction(tr("Invite to 1on1 chat"));
    connect(chatPeerInviteAction, SIGNAL(triggered()), this, SLOT(onCreatePeerChat()));

    auto chatInviteAction = chatMenu->addAction(tr("Invite to group chat"));
    connect(chatInviteAction, SIGNAL(triggered()), this, SLOT(onCreateGroupChat()));

    auto publicChatInviteAction = chatMenu->addAction(tr("Invite to PUBLIC group chat"));
    connect(publicChatInviteAction, SIGNAL(triggered()), this, SLOT(onCreatePublicGroupChat()));

    auto lastGreenAction = menu.addAction(tr("Last time user was online"));
    connect(lastGreenAction, SIGNAL(triggered()), this, SLOT(onRequestLastGreen()));

    QMenu *othersMenu = menu.addMenu("Others");

    auto printAction = othersMenu->addAction(tr("Print contact info"));
    connect(printAction, SIGNAL(triggered()), this, SLOT(onPrintContactInfo()));

    auto tooltipAction = othersMenu->addAction(tr("Update tooltip"));
    connect(tooltipAction, SIGNAL(triggered()), this, SLOT(onUpdateTooltip()));

    if (mUserVisibility == ::mega::MegaUser::VISIBILITY_VISIBLE)
    {
        auto removeAction = othersMenu->addAction(tr("Remove contact"));
        connect(removeAction, SIGNAL(triggered()), this, SLOT(onContactRemove()));
    }
    else if (mUserVisibility == ::mega::MegaUser::VISIBILITY_HIDDEN)
    {
        auto addAction = othersMenu->addAction(tr("Invite ex-contact"));
        connect(addAction, SIGNAL(triggered()), this, SLOT(onExContactInvite()));
    }

    auto aliasAction = othersMenu->addAction(tr("Set nickname"));
    connect(aliasAction, SIGNAL(triggered()), this, SLOT(onSetNickname()));

    menu.addSeparator();

    auto copyHandleAction = menu.addAction(tr("Copy to clipboard user id"));
    connect(copyHandleAction, SIGNAL(triggered()), this, SLOT(onCopyHandle()));

    menu.exec(event->globalPos());
    menu.deleteLater();
}

QListWidgetItem *ContactItemWidget::getWidgetItem() const
{
    return mListWidgetItem;
}

void ContactItemWidget::setWidgetItem(QListWidgetItem *item)
{
    mListWidgetItem = item;
}


void ContactItemWidget::updateToolTip(::mega::MegaUser *contact)
{
   QString text = NULL;
   std::string contactHandleBin = std::to_string(contact->getHandle());
   const char *email = contact->getEmail();
   const char *chatHandle_64 = "--------";
   const char *contactHandle_64 = mMegaApi->userHandleToBase64(contact->getHandle());
   const char *auxChatHandle_64 = mMegaApi->userHandleToBase64(mMegaChatApi->getChatHandleByUser(contact->getHandle()));

   if (mMegaChatApi->getChatHandleByUser(contact->getHandle()) != megachat::MEGACHAT_INVALID_HANDLE)
   {
      chatHandle_64 = auxChatHandle_64;
   }

   if (contact->getVisibility() == ::mega::MegaUser::VISIBILITY_HIDDEN)
   {
        text.append(tr("INVISIBLE:\n"));
   }

   text.append(tr("Email: "))
        .append(QString::fromStdString(email))
        .append(tr("\nUser handle bin: ")).append(contactHandleBin.c_str())
        .append(tr("\nUser handle: ")).append(contactHandle_64)
        .append(tr("\nChat handle: ")).append((chatHandle_64));

   setToolTip(text);
   delete [] contactHandle_64;
   delete [] auxChatHandle_64;
}

void ContactItemWidget::onCreatePublicGroupChat()
{
    createChatRoom(mUserHandle, true, true);
}

void ContactItemWidget::onCreateGroupChat()
{
    createChatRoom(mUserHandle, true, false);
}

void ContactItemWidget::onCreatePeerChat()
{
    createChatRoom(mUserHandle, false, false);
}

void ContactItemWidget::createChatRoom(MegaChatHandle uh, bool isGroup, bool isPublic)
{
    QMessageBox msgBox;
    if (isGroup)
    {
        msgBox.setText("Do you want to invite "+ui->mName->text() +" to a new group chat?");
    }
    else
    {
        msgBox.setText("Do you want to invite "+ui->mName->text() +" to a new 1on1 chat?");
    }
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    msgBox.setDefaultButton(QMessageBox::Cancel);

    if (msgBox.exec() == QMessageBox::Cancel)
    {
        return;
    }

    MegaChatPeerList *peerList = MegaChatPeerList::createInstance();
    peerList->addPeer(uh, MegaChatRoom::PRIV_STANDARD);


    if(isGroup)
    {
        char *title = mMainWin->askChatTitle();
        if (isPublic)
        {
            mMegaChatApi->createPublicChat(peerList, title);
        }
        else
        {
            mMegaChatApi->createChat(true, peerList, title);
        }
        delete [] title;
    }
    else
    {
        mMegaChatApi->createChat(false, peerList);
    }

    delete peerList;
}

void ContactItemWidget::onPrintContactInfo()
{
    QMessageBox msg;
    msg.setIcon(QMessageBox::Information);
    msg.setText(toolTip());
    msg.exec();
}

void ContactItemWidget::onContactRemove()
{
    char *email = mMegaChatApi->getContactEmail(mUserHandle);
    QString msg = tr("Are you sure you want to remove ");
    msg.append(ui->mName->text());

    if (ui->mName->text() != email)
    {
        msg.append(" (").append(email).append(")");
    }
    msg.append(tr(" from your contacts?"));

    auto ret = QMessageBox::question(this, tr("Remove contact"), msg);
    if (ret == QMessageBox::Yes)
    {
        ::mega::MegaUser *contact = mMegaApi->getContact(email);
        mMegaApi->removeContact(contact);
        delete contact;
    }
    delete [] email;
}

void ContactItemWidget::onExContactInvite()
{
    char *email = mMegaChatApi->getContactEmail(mUserHandle);
    QString msg = tr("Are you sure you want to re-invite ");
    msg.append(ui->mName->text());
    if (ui->mName->text() != email)
    {
        msg.append(" (").append(email).append(")");
    }
    msg.append(tr(" to your contacts?"));

    auto ret = QMessageBox::question(this, tr("Invite ex-contact"), msg);
    if (ret == QMessageBox::Yes)
    {
        mMegaApi->inviteContact(email, "Please, accept my invitation", ::mega::MegaContactRequest::INVITE_ACTION_ADD);
    }

    delete [] email;
}

void ContactItemWidget::onUpdateTooltip()
{
    if (mController)
    {
        updateToolTip(mController->getItem());
    }
}

void ContactItemWidget::onRequestLastGreen()
{
    mMegaChatApi->requestLastGreen(mUserHandle);
}

void ContactItemWidget::onCopyHandle()
{
    const char *handel_64 = mMegaApi->userHandleToBase64(mUserHandle);
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(handel_64);
    delete []handel_64;
}

void ContactItemWidget::onSetNickname()
{
    std::string nickname = mMainWin->mApp->getText("Set nickname: ", true);
    mMegaApi->setUserAlias(mUserHandle, nickname.c_str());
}

void ContactItemWidget::updateName(const char *name)
{
    mName = name ? name : std::string();
    updateTitle();
}

void ContactItemWidget::updateTitle()
{
    QString text;
    if (!mAlias.empty())
    {
        text = QString::fromUtf8(mAlias.c_str());
        if (!mName.empty())
        {
            text.append(" (")
            .append(QString::fromUtf8(mName.c_str()))
            .append(")");
        }
    }
    else if (!mName.empty())
    {
        text = QString::fromUtf8(mName.c_str());
    }
    else
    {
        const char *auxEmail = mMegaChatApi->getContactEmail(mUserHandle);
        text = QString::fromUtf8(auxEmail);
        delete [] auxEmail;
    }

    switch (mUserVisibility)
    {
        case ::mega::MegaUser::VISIBILITY_HIDDEN:
            text.append(" [H]");
            break;
        case ::mega::MegaUser::VISIBILITY_INACTIVE:
            text.append(" [I]");
            break;
        case ::mega::MegaUser::VISIBILITY_BLOCKED:
            text.append(" [B]");
            break;
        default:    // VISIBLE (+ UNKNOWN)
            break;
    }

    ui->mName->setText(text);
}

ContactItemWidget::~ContactItemWidget()
{
    delete ui;
}

void ContactItemWidget::updateOnlineIndicator(int newState)
{
    if (newState == megachat::MegaChatApi::STATUS_INVALID)
    {
        newState = 0;
    }

    if (newState >= 0 && newState < NINDCOLORS)
    {
        ui->mOnlineIndicator->setStyleSheet(
           QString("background-color: ")+gOnlineIndColors[newState]+
                   ";border-radius: 4px");
    }
}


