#ifndef CHATMESSAGE_H
#define CHATMESSAGE_H

#include <memory>
#include <QWidget>
#include <QDateTime>
#include <QListWidgetItem>
#include "megachatapi.h"
#include "ui_chatMessageWidget.h"
#include "chatWindow.h"
#include "reaction.h"

class ChatWindow;
class Reaction;
namespace Ui {
class ChatMessageWidget;
}

class ChatMessage: public QWidget
{
    Q_OBJECT
private:
    QString nodelistText();

protected:
        Ui::ChatMessageWidget *ui;
        megachat::MegaChatHandle mChatId;
        megachat::MegaChatMessage *mMessage = NULL;
        megachat::MegaChatApi *megaChatApi;
        QListWidgetItem *mListWidgetItem;
        QWidget *mReactWidget;
        QHBoxLayout *mReactLayout;
        ChatWindow *mChatWindow;
        friend class ChatWindow;

    public:
        ChatMessage(ChatWindow *window, megachat::MegaChatApi *mChatApi, megachat::MegaChatHandle mChatId, megachat::MegaChatMessage *msg);
        virtual ~ChatMessage();
        std::string managementInfoToString() const;
        void updateToolTip();
        void showContainsMetaData();
        void setMessageContent(const char *content);
        void updateContent();
        void setTimestamp(int64_t ts);
        void setStatus(int status);
        void setAuthor(const char *author);
        bool isMine() const;
        void markAsEdited();
        void startEditingMsgWidget();
        QListWidgetItem *getWidgetItem() const;
        void setWidgetItem(QListWidgetItem *item);
        megachat::MegaChatMessage *getMessage() const;
        void setMessage(megachat::MegaChatMessage *message);
        void clearEdit();
        void setManualMode(bool manualMode);
        ChatWindow *getChatWindow() const;
        megachat::MegaChatApi *getMegaChatApi() const;
        megachat::MegaChatHandle getChatId() const;
        void updateReaction(const char *reactionStr, int count);
        const Reaction *getLocalReaction(const char *reactionStr) const;
        void clearReactions();

    public slots:
        void onDiscardManualSending();
        void onManualSending();
        void cancelMsgEdit(bool clicked);
        void saveMsgEdit(bool clicked);
        void onMessageCtxMenu(const QPoint& point);
        void onMessageDelAction();
        void onMessageEditAction();
        void onReactCount();
        void onManageReaction(bool del, const char *reactionStr = nullptr);
        void onMessageRemoveLinkAction();
        void onNodeDownloadOrImport(mega::MegaNode *node, bool import);
        void onNodePlay(::mega::MegaNode *node);
        void on_bSettings_clicked();
        void onCopyHandle();
};
#endif // CHATMESSAGE_H
