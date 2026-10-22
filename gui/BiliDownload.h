#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bili {

struct VideoInfo {
    std::string title;      // UTF-8, as returned by the API
    std::string bvid;       // empty when the link is av-only
    long long aid = 0;
    long long cid = 0;
    int durationSec = 0;
};

// stage = Chinese message ("解析链接", "下载视频流 12.3/80.0 MB", "合并音视频"...).
// fraction = 0.0..1.0, or -1 when unknown. Called from the calling thread only.
using ProgressFn = std::function<void(const std::string& stage, double fraction)>;

// True for http(s) bilibili video links (bilibili.com/video/BV..|av.., b23.tv/.., bare BV..id).
bool looksLikeBiliUrl(const std::string& input);

// Blocking: resolve URL (follow b23.tv redirects, view API) -> info. err = UTF-8 Chinese message.
bool resolve(const std::string& url, VideoInfo& info, std::string& err,
             const ProgressFn& progress, std::atomic<bool>* cancel);

// Blocking full pipeline: resolve + playurl (DASH) + stream download via httplib + ffmpeg merge.
// Creates outDir if needed. Produces outDir/<sanitized title>.mp4; temp *.Video.*/*.Audio.*
// intermediates removed on success. outFileOut = UTF-8 path of the mp4.
// cancel may be nullptr; when non-null it is polled during stream download.
bool download(const std::string& url, const std::filesystem::path& outDir,
              const std::filesystem::path& ffmpegExe, VideoInfo& info,
              std::filesystem::path& outFileOut, std::string& err,
              const ProgressFn& progress, std::atomic<bool>* cancel);

// ---------------- Cookie / login (process-wide, thread-safe) ----------------
// Cookie string form: "SESSDATA=..; bili_jct=..; DedeUserID=..". Empty = anonymous.
// Persisted by the GUI (QSettings); the module itself keeps it in memory only.
void setCookie(const std::string& cookie);
std::string getCookie();

// QR login: GET passport.bilibili.com/x/passport-login/web/qrcode/generate
// -> qrUrl (encode THIS into the QR image), qrKey for polling.
bool qrStart(std::string& qrUrl, std::string& qrKey, std::string& err);

enum class QrPoll {
    Waiting,    // code 86101: not scanned yet
    Scanned,    // code 86090: scanned, waiting confirm
    Success,    // code 0: Set-Cookie captured into the module cookie store
    Expired,    // code 86038: expired (caller should qrStart again)
    Error,      // network/JSON failure; err filled with Chinese message
};
// Single non-blocking poll of .../qrcode/poll?qrcode_key=.. (use a ~2s timer in the GUI).
QrPoll qrPoll(const std::string& qrKey, std::string& err);

// GET api.bilibili.com/x/web-interface/nav with the current cookie.
// loggedIn=false with err=="" is NOT an error (anonymous). uname only valid when loggedIn.
// face (optional): filled with data.face (头像 URL) when loggedIn.
bool navLoginState(bool& loggedIn, std::string& uname, std::string& err, std::string* face = nullptr);

// GET an avatar/image URL (bing/hdslb CDN). bytes = raw image data.
bool fetchAvatar(const std::string& url, std::string& bytes, std::string& err);

// ---------------- Private messages (私信, message/private_msg.md) ----------------
// All blocking; call from worker threads; err = UTF-8 Chinese message.
// GET endpoints use the cookie (SESSDATA); sendPmText additionally needs
// csrf (bili_jct cookie) and a Wbi signature. sendPmText also marks the
// target session read server-side (see 发送私信 doc note).

struct PmSession {
    long long talkerId = 0;   // peer mid (session_type is always 1 here)
    int unread = 0;
    long long ackSeqno = 0;   // newest seqno already read
    long long maxSeqno = 0;   // newest seqno in the session
    long long lastSender = 0; // last_msg.sender_uid
    int lastMsgType = 0;
};

struct PmMessage {
    long long senderUid = 0;
    long long seqno = 0;
    long long timestamp = 0;  // seconds
    int msgType = 0;
    int msgStatus = 0;        // 0 = normal; skip 1/2 (recalled)
    std::string content;      // raw `content` field (JSON-encoded payload)
};

// GET api.vc.bilibili.com/session_svr/v1/session_svr/get_sessions
//   ?session_type=4&size=50&sort_rule=1&build=0&mobi_app=web
// Returns ONLY user sessions (session_type==1 && system_msg_type==0),
// newest activity first, capped at 50.
bool fetchPmSessions(std::vector<PmSession>& out, std::string& err);

// GET api.vc.bilibili.com/svr_sync/v1/svr_sync/fetch_session_msgs
//   ?talker_id=<uid>&session_type=1&size=<size>&begin_seqno=<begin>
//   &sender_device_id=1&build=0&mobi_app=web
// Returns messages with seqno > beginSeqno, NEWEST FIRST (reverse order);
// size clamped to 1..200.
bool fetchPmMessages(long long talkerId, long long beginSeqno, int size,
                     std::vector<PmMessage>& out, std::string& err);

// POST api.vc.bilibili.com/web_im/v1/web_im/send_msg (Wbi-signed, csrf).
// Sends UTF-8 text (msg_type=1) to user mid; text is byte-truncated to fit
// the 2000-byte limit. Server marks the session read on success.
bool sendPmText(long long receiverUid, const std::string& text, std::string& err);

// POST api.vc.bilibili.com/session_svr/v1/session_svr/update_ack (csrf).
// Marks the peer session read through ackSeqno (0 = up to the newest).
bool markPmRead(long long talkerId, long long ackSeqno, std::string& err);

// Own mid: api.bilibili.com nav data.mid, falling back to the DedeUserID
// cookie. false + err when neither is available (not logged in).
bool myMid(long long& mid, std::string& err);

// ---------------- 通知消息 (回复我的 / @我, message/msg.md) ----------------

// Unread counters of the NEW endpoint (the old api.bilibili.com/x/msgfeed/
// unread is not used). This endpoint does NOT report private-message counts.
struct MsgFeedUnread {
    int at = 0;         // 未读 @ 我的数量
    int reply = 0;      // 未读回复数
    int recvReply = 0;  // 未读回复与 @ 数 (= reply + at)
};

// One entry of data.items of "@我的" (被@通知, x/msgfeed/at). Kept in API
// order (newest first); the caller dedupes by id.
struct NoticeUser {
    long long mid = 0;     // 用户 mid
    std::string nickname;  // 昵称
    bool follow = false;   // 是否关注该用户
};

struct ReplyNotice {
    long long id = 0;         // 通知 id
    long long userMid = 0;    // 发出该通知的用户 mid (Job::talkerId 的来源)
    std::string nickname;     // 该用户昵称
    bool follow = false;      // 是否关注该用户
    long long atTime = 0;     // at_time 被@时间 (unix 秒)
    std::string title;        // item.title 被@视频/动态的标题
    std::string uri;          // item.uri web 端跳转链接
    std::string nativeUri;    // item.native_uri 客户端跳转链接
    std::string image;        // item.image 封面图 URL
    std::string type;         // item.type 如 "reply"
    std::string business;     // item.business "评论" / "视频" 等
    long long businessId = 0; // item.business_id 1=视频评论, 11=动态评论
    long long subjectId = 0;  // item.subject_id 主体 id (视频为 oid)
    long long rootId = 0;     // item.root_id 最顶层评论 id
    long long targetId = 0;   // item.target_id 被回复的评论 id
    long long sourceId = 0;   // item.source_id 直接回复的评论 id
    std::string detailTitle;  // item.detail_title 详细标题 (通常为空)
    std::string rootReplyContent;   // item.root_reply_content 最顶层评论内容
    std::string sourceContent;      // item.source_content 直接回复的评论内容
    std::string targetReplyContent; // item.target_reply_content 被回复的评论内容
    std::vector<NoticeUser> atDetails;  // item.at_details 被@的用户列表
};

// GET api.vc.bilibili.com/x/im/web/msgfeed/unread?build=0&mobi_app=web
bool fetchMsgFeedUnread(MsgFeedUnread& out, std::string& err);

// GET api.bilibili.com/x/msgfeed/at?platform=web&build=0&mobi_app=web
bool fetchReplyNotices(std::vector<ReplyNotice>& out, std::string& err);

// 批量查询"对方是否关注了我" (message/docs/user/relation.md 查询用户与自己关系):
//   接口2  GET api.bilibili.com/x/web-interface/relation?mid=<目标mid>  (逐个查询, 免 Wbi)
//   接口1  GET api.bilibili.com/x/space/wbi/acc/relation?mid=<目标mid>  (需 Wbi 签名, 未使用)
// 回复 data.relation  = 目标用户对于当前用户的关系  <- 本函数取这个 (对方是否关注了我)
//      data.be_relation = 当前用户对于目标用户的关系 (我是否关注对方, 未使用)
// attribute: 0=未关注 1=悄悄关注(已弃用) 2=已关注 6=互粉 128=拉黑
// followsMe[mid] = attribute>0 && attribute<128。
// 任一查询失败即返回 false (调用方应跳过本轮, 不要回退到 msgfeed 的 follow 字段)。
bool fetchRelations(const std::vector<long long>& mids,
                    std::unordered_map<long long, bool>& followsMe,
                    std::string& err);

// Best-effort video link extraction from a received message:
//   msg_type 1 -> first whitespace-delimited token of the inner text that
//                 looksLikeBiliUrl() accepts (http link or bare BV id);
//   msg_type 7 -> content.url when source==5, else "https://www.bilibili.com/video/<bvid>";
//   otherwise  -> "".
std::string pmVideoLink(const PmMessage& m);

}  // namespace bili
