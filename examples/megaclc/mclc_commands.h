#ifndef MCLC_COMMANDS_H
#define MCLC_COMMANDS_H

/**
 * @file
 * @brief File with all the definitions of the commands that are exposed to the cli.
 *
 * NOTE: Remember to attach the function you define here to a command in the body of the
 * autocompleteSyntax function defined in mclc_autocompletion.cpp
 */

#include <mega/autocomplete.h>
namespace ac = ::mega::autocomplete;

namespace mclc::clc_cmds
{

void exec_initanonymous(ac::ACState&);
void exec_login(ac::ACState& s);
void exec_logout(ac::ACState&);
void exec_session(ac::ACState& s);
void exec_debug(ac::ACState& s);
void exec_easy_debug(ac::ACState& s);
void exec_setonlinestatus(ac::ACState& s);
void exec_setpresenceautoaway(ac::ACState& s);
void exec_setpresencepersist(ac::ACState& s);
void exec_signalpresenceperiod(ac::ACState& s);
void exec_repeat(ac::ACState& s);
void exec_getonlinestatus(ac::ACState&);
void exec_setbackgroundstatus(ac::ACState& s);
void exec_getuserfirstname(ac::ACState& s);
void exec_getuserlastname(ac::ACState& s);
void exec_getuseremail(ac::ACState& s);
void exec_getcontactemail(ac::ACState& s);
void exec_getuserhandlebyemail(ac::ACState& s);
void exec_getmyuserhandle(ac::ACState&);
void exec_getmyfirstname(ac::ACState&);
void exec_getmylastname(ac::ACState&);
void exec_getmyfullname(ac::ACState&);
void exec_getmyemail(ac::ACState&);
void exec_getchatrooms(ac::ACState&);
void exec_getchatroom(ac::ACState& s);
void exec_getchatroombyuser(ac::ACState& s);
void exec_getchatlistitems(ac::ACState&);
void exec_getchatlistitem(ac::ACState& s);
void exec_getunreadchats(ac::ACState&);
void exec_getactivechatlistitems(ac::ACState&);
void exec_getinactivechatlistitems(ac::ACState&);
void exec_getunreadchatlistitems(ac::ACState&);
void exec_chatinfo(ac::ACState& s);
void exec_getchathandlebyuser(ac::ACState& s);
void exec_createchat(ac::ACState& s);
void exec_invitetochat(ac::ACState& s);
void exec_removefromchat(ac::ACState& s);
void exec_leavechat(ac::ACState& s);
void exec_updatechatpermissions(ac::ACState& s);
void exec_truncatechat(ac::ACState& s);
void exec_clearchathistory(ac::ACState& s);
void exec_setRetentionTime(ac::ACState& s);
void exec_getRetentionTime(ac::ACState& s);
void exec_setchattitle(ac::ACState& s);
void exec_openchatroom(ac::ACState& s);
void exec_closechatroom(ac::ACState& s);
void exec_openchatpreview(ac::ACState& s);
void exec_closechatpreview(ac::ACState& s);
void exec_loadmessages(ac::ACState& s);
void exec_dumpchathistory(ac::ACState& s);
void exec_reviewpublicchat(ac::ACState& s);
void exec_isfullhistoryloaded(ac::ACState& s);
void exec_getmessage(ac::ACState& s);
void exec_getmanualsendingmessage(ac::ACState& s);
void exec_sendmessage(ac::ACState& s);
void exec_attachcontacts(ac::ACState& s);
void exec_attachnode(ac::ACState& s);
void exec_revokeattachmentmessage(ac::ACState& s);
void exec_editmessage(ac::ACState& s);
void exec_deletemessage(ac::ACState& s);
void exec_setmessageseen(ac::ACState& s);
void exec_getLastMessageSeen(ac::ACState& s);
void exec_removeunsentmessage(ac::ACState& s);
void exec_sendtypingnotification(ac::ACState& s);
void exec_ismessagereceptionconfirmationactive(ac::ACState&);
void exec_savecurrentstate(ac::ACState&);
void exec_detail(ac::ACState& s);
void exec_dos_unix(ac::ACState& s);
void exec_help(ac::ACState&);
void exec_history(ac::ACState& s);
void exec_quit(ac::ACState&);
#ifndef KARERE_DISABLE_WEBRTC
void exec_joinCallViaMeetingLink(ac::ACState& s);
void exec_getchatvideoindevices(ac::ACState&);
void exec_setchatvideoindevice(ac::ACState& s);
void exec_startchatcall(ac::ACState& s);
void exec_answerchatcall(ac::ACState& s);
void exec_hangchatcall(ac::ACState& s);
void exec_enableaudio(ac::ACState& s);
void exec_disableaudio(ac::ACState& s);
void exec_enablevideo(ac::ACState& s);
void exec_disablevideo(ac::ACState& s);
void exec_getchatcall(ac::ACState&);
void exec_setignoredcall(ac::ACState& s);
void exec_getchatcallbycallid(ac::ACState&);
void exec_getnumcalls(ac::ACState&);
void exec_getchatcalls(ac::ACState&);
void exec_getchatcallsids(ac::ACState&);
#endif // KARERE_DISABLE_WEBRTC
void exec_smsverify(ac::ACState& s);
void exec_apiurl(ac::ACState& s);
void exec_catchup(ac::ACState& s);
void exec_backgroundupload(ac::ACState& s);
void exec_setthumbnailbyhandle(ac::ACState& s);
void exec_setpreviewbyhandle(ac::ACState& s);
void exec_ensuremediainfo(ac::ACState&);
void exec_getfingerprint(ac::ACState& s);
void exec_createthumbnail(ac::ACState& s);
void exec_createpreview(ac::ACState& s);
void exec_getthumbnail(ac::ACState& s);
void exec_cancelgetthumbnail(ac::ACState& s);
void exec_getpreview(ac::ACState& s);
void exec_cancelgetpreview(ac::ACState& s);
void exec_testAllocation(ac::ACState& s);
void exec_recentactions(ac::ACState& s);
void exec_getspecificaccountdetails(ac::ACState& s);
void exec_setnodecoordinates(ac::ACState& s);
void exec_setunshareablenodecoordinates(ac::ACState& s);
void exec_getnodebypath(ac::ACState& s);
void exec_ls(ac::ACState& s);
void exec_renamenode(ac::ACState& s);
void exec_createfolder(ac::ACState& s);
void exec_remove(ac::ACState& s);
void exec_cancelbytoken(ac::ACState& s);
void exec_setmaxuploadspeed(ac::ACState& s);
void exec_setmaxdownloadspeed(ac::ACState& s);
void exec_startupload(ac::ACState& s);
void exec_startdownload(ac::ACState& s);
void exec_pausetransfers(ac::ACState& s);
void exec_pausetransferbytag(ac::ACState& s);
void exec_canceltransfers(ac::ACState& s);
void exec_canceltransferbytag(ac::ACState& s);
void exec_gettransfers(ac::ACState& s);
void exec_exportNode(ac::ACState& s);
void exec_pushreceived(ac::ACState& s);
void exec_getcloudstorageused(ac::ACState&);
void exec_cp(ac::ACState& s);
void exec_mv(ac::ACState& s);
void exec_getaccountachievements(ac::ACState&);
void exec_getmegaachievements(ac::ACState&);
void exec_setCameraUploadsFolder(ac::ACState& s);
void exec_getCameraUploadsFolder(ac::ACState&);
void exec_setCameraUploadsFolderSecondary(ac::ACState& s);
void exec_getCameraUploadsFolderSecondary(ac::ACState&);
void exec_getContact(ac::ACState& s);
void exec_getDefaultTZ(ac::ACState& s);
void exec_isGeolocOn(ac::ACState& s);
void exec_setGeolocOn(ac::ACState& s);
void exec_treecompare(ac::ACState& s);
void exec_generatetestfilesfolders(ac::ACState& s);
void exec_syncadd(ac::ACState& s);
void exec_syncclosedrive(ac::ACState& s);
void exec_syncexport(ac::ACState& s);
void exec_syncimport(ac::ACState& s);
void exec_syncopendrive(ac::ACState& s);
void exec_synclist(ac::ACState&);
void exec_syncremove(ac::ACState& s);
void exec_syncxable(ac::ACState& s);
void exec_getmybackupsfolder(ac::ACState&);
void exec_setmybackupsfolder(ac::ACState& s);
}
#endif // MCLC_COMMANDS_H
