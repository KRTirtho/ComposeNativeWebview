// Chromium browser-process host. This executable, never the JVM, owns CEF's
// one initialization/shutdown epoch. Reopening starts another executable.
#include "host_protocol.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_browser.h"
#include "include/cef_render_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_request_handler.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_command_ids.h"
#include "include/cef_cookie.h"
#include "include/cef_image.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include <gtk/gtk.h>
#include <fcntl.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
#include <deque>
#include <charconv>
#include <cmath>

using namespace cefipc;
namespace {
int channel;
std::mutex output_mutex, exit_mutex, navigation_mutex;
std::condition_variable exit_condition, navigation_condition;
std::atomic<bool> stopping{false};
std::atomic<int> live_views{0};
uint64_t next_navigation = 0;
std::map<uint64_t, bool> navigation_replies;
void output(Op op, uint64_t view, const Writer &w = {}) {
    std::lock_guard lock(output_mutex); sendPacket(channel, op, view, w.data);
}
void text(Op op, uint64_t id, const std::string &s) { Writer w; w.text(s); output(op, id, w); }
class Task final : public CefTask {
public:
    explicit Task(std::function<void()> f) : f_(std::move(f)) {}
    void Execute() override { f_(); }
private:
    std::function<void()> f_; IMPLEMENT_REFCOUNTING(Task);
};
void post(std::function<void()> f) { CefPostTask(TID_UI, new Task(std::move(f))); }
struct View {
    uint64_t id;
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefRequestContext> context;
    std::string url, title, init, bridge, ua;
    int width = 800, height = 600;
    bool closed = false, document_ready = false, js = true;
    double zoom = 1;
    std::deque<Packet> pending;
    std::map<uint64_t, bool> evaluations;
    uint64_t next_eval = 0, menu_token = 0;
    CefRefPtr<CefRunContextMenuCallback> menu_callback;
    CefRefPtr<CefFrame> menu_frame;
    std::string menu_link, menu_source;
    CefRequest::HeaderMap headers;
    std::string header_url;
    std::mutex header_mutex;
};
std::map<uint64_t, std::shared_ptr<View>> views; // CEF UI thread only
void command(Packet p);
void flush(const std::shared_ptr<View> &s) {
    auto pending = std::move(s->pending); s->pending.clear();
    for (auto &p : pending) command(std::move(p));
}
void status(const std::shared_ptr<View> &s) {
    Writer w; w.text(s->url); w.text(s->title);
    w.integer(s->browser && s->browser->IsLoading());
    w.integer(s->browser && s->browser->CanGoBack()); w.integer(s->browser && s->browser->CanGoForward());
    output(Op::Status, s->id, w);
}
bool allowNavigation(const std::shared_ptr<View> &s, const std::string &url) {
    uint64_t token;
    { std::lock_guard lock(navigation_mutex); token = ++next_navigation; }
    Writer w; w.value(token); w.text(url); output(Op::NavigateRequest, s->id, w);
    std::unique_lock lock(navigation_mutex);
    navigation_condition.wait_for(lock, std::chrono::seconds(10), [&] { return navigation_replies.count(token) || stopping; });
    auto i = navigation_replies.find(token); bool allow = i != navigation_replies.end() && i->second;
    if (i != navigation_replies.end()) navigation_replies.erase(i);
    return allow;
}
std::string quote(const std::string &s) {
    auto value = CefValue::Create(); value->SetString(s); return CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString();
}
std::string b64(const std::string &s) { return CefBase64Encode(s.data(), s.size()).ToString(); }
void copySelection(const std::shared_ptr<View> &s, CefRefPtr<CefFrame> frame, bool cut) {
    auto script = std::string("(function(){let e=document.activeElement;let t=e&&typeof e.selectionStart==='number'?e.value.slice(e.selectionStart,e.selectionEnd):String(window.getSelection()||'');console.log('__compose_cef_clip:'+btoa(unescape(encodeURIComponent(t))));") +
        (cut ? "if(t)document.execCommand('delete');" : "") + "})();";
    frame->ExecuteJavaScript(script, frame->GetURL(), 0);
}
class ImageCopy final : public CefDownloadImageCallback {
public:
    explicit ImageCopy(uint64_t id) : id_(id) {}
    void OnDownloadImageFinished(const CefString &, int status, CefRefPtr<CefImage> image) override {
        if (!image || image->IsEmpty()) return;
        int width, height; auto png = image->GetAsPNG(1, true, width, height); if (!png) return;
        Writer w; w.data.resize(png->GetSize()); png->GetData(w.data.data(), w.data.size(), 0); output(Op::ClipboardImage, id_, w);
    }
private:
    uint64_t id_; IMPLEMENT_REFCOUNTING(ImageCopy);
};
constexpr int kLink = MENU_ID_USER_FIRST, kImageUrl = MENU_ID_USER_FIRST + 1, kImage = MENU_ID_USER_FIRST + 2;
bool allowed(int id) {
    switch (id) {
        case MENU_ID_UNDO: case MENU_ID_REDO: case MENU_ID_CUT: case MENU_ID_COPY: case MENU_ID_PASTE:
        case MENU_ID_PASTE_MATCH_STYLE: case MENU_ID_DELETE: case MENU_ID_SELECT_ALL:
        case MENU_ID_BACK: case MENU_ID_FORWARD: case MENU_ID_RELOAD:
        case IDC_CONTENT_CONTEXT_UNDO: case IDC_CONTENT_CONTEXT_REDO: case IDC_CONTENT_CONTEXT_CUT:
        case IDC_CONTENT_CONTEXT_COPY: case IDC_CONTENT_CONTEXT_PASTE: case IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE:
        case IDC_CONTENT_CONTEXT_DELETE: case IDC_CONTENT_CONTEXT_SELECTALL: case IDC_BACK: case IDC_FORWARD:
        case IDC_RELOAD: case IDC_CONTENT_CONTEXT_OPENLINKNEWTAB: case IDC_CONTENT_CONTEXT_COPYLINKLOCATION:
        case IDC_CONTENT_CONTEXT_COPYIMAGELOCATION: case IDC_CONTENT_CONTEXT_COPYIMAGE: case kLink: case kImageUrl: case kImage: return true;
        default: return false;
    }
}
std::vector<MenuEntry> menuEntries(CefRefPtr<CefMenuModel> model) {
    std::vector<MenuEntry> result;
    for (size_t i = 0; i < model->GetCount(); ++i) {
        if (!model->IsVisibleAt(i)) continue;
        MenuEntry e; e.type = model->GetTypeAt(i); e.command = model->GetCommandIdAt(i);
        e.enabled = model->IsEnabledAt(i); e.checked = model->IsCheckedAt(i);
        auto label = model->GetLabelAt(i).ToString();
        for (char c : label) if (c != '&') e.label += c;
        if (e.type == MENUITEMTYPE_SUBMENU) { e.children = menuEntries(model->GetSubMenuAt(i)); if (e.children.empty()) continue; }
        else if (e.type == MENUITEMTYPE_SEPARATOR) { if (result.empty() || result.back().type == MENUITEMTYPE_SEPARATOR) continue; }
        else if (!allowed(e.command)) continue;
        result.push_back(std::move(e));
    }
    while (!result.empty() && result.back().type == MENUITEMTYPE_SEPARATOR) result.pop_back();
    return result;
}
void edit(const std::shared_ptr<View> &s, int id, const std::string &paste = {}, CefRefPtr<CefFrame> frame = nullptr) {
    if (!s->browser) return;
    if (!frame) frame = s->browser->GetFocusedFrame();
    if (!frame) frame = s->browser->GetMainFrame();
    if (!frame) return;
    switch (id) {
        case MENU_ID_UNDO: case IDC_CONTENT_CONTEXT_UNDO: frame->Undo(); break;
        case MENU_ID_REDO: case IDC_CONTENT_CONTEXT_REDO: frame->Redo(); break;
        case MENU_ID_SELECT_ALL: case IDC_CONTENT_CONTEXT_SELECTALL: frame->SelectAll(); break;
        case MENU_ID_DELETE: case IDC_CONTENT_CONTEXT_DELETE: frame->Delete(); break;
        case MENU_ID_COPY: case IDC_CONTENT_CONTEXT_COPY: copySelection(s, frame, false); break;
        case MENU_ID_CUT: case IDC_CONTENT_CONTEXT_CUT: copySelection(s, frame, true); break;
        case MENU_ID_PASTE: case IDC_CONTENT_CONTEXT_PASTE: case MENU_ID_PASTE_MATCH_STYLE: case IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE:
            frame->ExecuteJavaScript("document.execCommand('insertText',false," + quote(paste) + ");", frame->GetURL(), 0); break;
        case MENU_ID_BACK: case IDC_BACK: s->browser->GoBack(); break;
        case MENU_ID_FORWARD: case IDC_FORWARD: s->browser->GoForward(); break;
        case MENU_ID_RELOAD: case IDC_RELOAD: s->browser->Reload(); break;
        case kLink: case IDC_CONTENT_CONTEXT_COPYLINKLOCATION: text(Op::ClipboardText, s->id, s->menu_link); break;
        case kImageUrl: case IDC_CONTENT_CONTEXT_COPYIMAGELOCATION: text(Op::ClipboardText, s->id, s->menu_source); break;
        case kImage: case IDC_CONTENT_CONTEXT_COPYIMAGE: s->browser->GetHost()->DownloadImage(s->menu_source, false, 0, false, new ImageCopy(s->id)); break;
        case IDC_CONTENT_CONTEXT_OPENLINKNEWTAB: frame->LoadURL(s->menu_link); break;
    }
}
class Headers final : public CefResourceRequestHandler {
public:
    explicit Headers(CefRequest::HeaderMap headers) : headers_(std::move(headers)) {}
    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, CefRefPtr<CefCallback>) override {
        CefRequest::HeaderMap h;
        request->GetHeaderMap(h);
        for (const auto &[name, value] : headers_) {
            for (auto it = h.begin(); it != h.end();) {
                if (g_ascii_strcasecmp(it->first.ToString().c_str(), name.ToString().c_str()) == 0) it = h.erase(it);
                else ++it;
            }
            h.insert({name, value});
        }
        request->SetHeaderMap(h);
        return RV_CONTINUE;
    }
private:
    CefRequest::HeaderMap headers_; IMPLEMENT_REFCOUNTING(Headers);
};
class Client final : public CefClient, public CefRenderHandler, public CefLifeSpanHandler,
    public CefDisplayHandler, public CefLoadHandler, public CefRequestHandler, public CefContextMenuHandler {
public:
    explicit Client(std::shared_ptr<View> s) : s_(std::move(s)) {}
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) override { rect = CefRect(0, 0, std::max(1,s_->width), std::max(1,s_->height)); }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &, const void *buffer, int width, int height) override {
        if (type != PET_VIEW || s_->closed || uint64_t(width) * height * 4 > kMaxPacket - 8) return;
        Writer w; w.integer(width); w.integer(height);
        auto *bytes = static_cast<const uint8_t *>(buffer); w.data.insert(w.data.end(), bytes, bytes + size_t(width) * height * 4); output(Op::Paint, s_->id, w);
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        s_->browser = browser;
        if (s_->closed || stopping) browser->GetHost()->CloseBrowser(true);
        else { browser->GetHost()->SetZoomLevel(s_->zoom == 1 ? 0 : s_->zoom); post([s=s_] { flush(s); }); }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        if (s_->menu_callback) { s_->menu_callback->Cancel(); s_->menu_callback = nullptr; }
        s_->browser = nullptr; views.erase(s_->id); live_views--; output(Op::Closed, s_->id); exit_condition.notify_all();
    }
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int, const CefString &url,
        const CefString &, CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures &, CefWindowInfo &,
        CefRefPtr<CefClient> &, CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *) override {
        if (!url.empty()) post([browser,url] { if (auto f=browser->GetMainFrame()) f->LoadURL(url); });
        return true;
    }
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString &url) override {
        if (frame->IsMain()) { s_->url = url.ToString(); status(s_); }
    }
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString &title) override { s_->title=title.ToString(); status(s_); }
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool, bool, bool) override { status(s_); }
    void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, TransitionType) override {
        if (frame->IsMain()) {
            s_->document_ready = false;
            for (size_t i = 0; i < s_->evaluations.size(); ++i) text(Op::JsResult,s_->id,"");
            s_->evaluations.clear();
        }
    }
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
        if (frame->IsMain()) { s_->document_ready=true; post([s=s_] { flush(s); }); }
    }
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus, int,
                                   const CefString &error) override {
        s_->document_ready = false;
        for (size_t i = 0; i < s_->evaluations.size(); ++i) text(Op::JsResult, s_->id, "");
        s_->evaluations.clear();
        text(Op::Error, s_->id, "renderer terminated: " + error.ToString());
    }
    bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request, bool, bool) override {
        return s_->closed || !allowNavigation(s_,request->GetURL().ToString());
    }
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
        CefRefPtr<CefRequest> request, bool, bool, const CefString &, bool &) override {
        CefRequest::HeaderMap h;
        {
            std::lock_guard lock(s_->header_mutex);
            if (request->GetURL().ToString()==s_->header_url) h=s_->headers;
        }
        if (!s_->ua.empty()) h.insert({"User-Agent",s_->ua});
        return h.empty() ? nullptr : CefRefPtr<CefResourceRequestHandler>(new Headers(std::move(h)));
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString &msg, const CefString &, int) override {
        auto str=msg.ToString();
        if (str.starts_with("__compose_cef_eval:")) {
            auto pos=str.find(':',19); if(pos==std::string::npos) return true;
            uint64_t id = 0;
            auto parsed = std::from_chars(str.data() + 19, str.data() + pos, id);
            if (parsed.ec == std::errc() && s_->evaluations.erase(id)) text(Op::JsResult,s_->id,str.substr(pos+1));
            return true;
        }
        if(str.starts_with("__compose_cef_clip:")) {
            auto data=CefBase64Decode(str.substr(19)); if(data) { std::string result(data->GetSize(),'\0'); data->GetData(result.data(),result.size(),0); text(Op::ClipboardText,s_->id,result); } return true;
        }
        if(str.starts_with("__compose_cef_system_clipboard:")) {
            auto data=CefBase64Decode(str.substr(31)); if(data) { std::string result(data->GetSize(),'\0'); data->GetData(result.data(),result.size(),0); text(Op::ClipboardText,s_->id,result); } return true;
        }
        return false;
    }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefProcessId, CefRefPtr<CefProcessMessage> msg) override {
        if(msg->GetName()=="compose_cef_ipc") { text(Op::Ipc,s_->id,msg->GetArgumentList()->GetString(0)); return true; } return false;
    }
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model) override {
        if(!menuEntries(model).empty()) return;
        model->Clear();
        if(params->IsEditable()) {
            int flags=params->GetEditStateFlags();
            auto add=[&](int id,const char *label,int flag){model->AddItem(id,label);model->SetEnabled(id,(flags&flag)!=0);};
            add(MENU_ID_UNDO,"Undo",CM_EDITFLAG_CAN_UNDO);add(MENU_ID_REDO,"Redo",CM_EDITFLAG_CAN_REDO);model->AddSeparator();
            add(MENU_ID_CUT,"Cut",CM_EDITFLAG_CAN_CUT);add(MENU_ID_COPY,"Copy",CM_EDITFLAG_CAN_COPY);add(MENU_ID_PASTE,"Paste",CM_EDITFLAG_CAN_PASTE);
            add(MENU_ID_DELETE,"Delete",CM_EDITFLAG_CAN_DELETE);model->AddSeparator();add(MENU_ID_SELECT_ALL,"Select All",CM_EDITFLAG_CAN_SELECT_ALL);
        } else if(params->GetTypeFlags()&CM_TYPEFLAG_SELECTION) model->AddItem(MENU_ID_COPY,"Copy");
        if(!params->GetUnfilteredLinkUrl().empty()) model->AddItem(kLink,"Copy link address");
        if(params->GetMediaType()==CM_MEDIATYPE_IMAGE) {model->AddItem(kImageUrl,"Copy image address");model->AddItem(kImage,"Copy image");}
        if(!model->GetCount()) {model->AddItem(MENU_ID_BACK,"Back");model->AddItem(MENU_ID_FORWARD,"Forward");model->AddItem(MENU_ID_RELOAD,"Reload");}
    }
    bool RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model, CefRefPtr<CefRunContextMenuCallback> callback) override {
        s_->menu_callback=callback; s_->menu_frame=frame; s_->menu_link=params->GetUnfilteredLinkUrl().ToString(); s_->menu_source=params->GetSourceUrl().ToString();
        Writer w; w.value(++s_->menu_token); w.integer(params->GetXCoord()); w.integer(params->GetYCoord()); encodeMenu(w,menuEntries(model)); output(Op::Menu,s_->id,w); return true;
    }
private:
    std::shared_ptr<View> s_; IMPLEMENT_REFCOUNTING(Client);
};
class Cookies final : public CefCookieVisitor {
public:
    explicit Cookies(uint64_t id) : id_(id) {}
    bool Visit(const CefCookie &c, int, int, bool &) override {
        if (!json_.empty()) json_ += ",";
        json_ += "{\"name\":" + quote(CefString(&c.name)) + ",\"value\":" + quote(CefString(&c.value)) +
            ",\"domain\":" + quote(CefString(&c.domain)) + ",\"path\":" + quote(CefString(&c.path)) +
            ",\"secure\":" + (c.secure ? "true" : "false") + ",\"httpOnly\":" + (c.httponly ? "true" : "false") +
            ",\"sessionOnly\":" + (c.has_expires ? "false" : "true") + ",\"expiresDate\":" +
            std::to_string(c.has_expires ? (c.expires.val - 11644473600000000LL) / 1000 : 0) +
            ",\"sameSite\":" + quote(c.same_site == CEF_COOKIE_SAME_SITE_STRICT_MODE ? "Strict" :
                                     c.same_site == CEF_COOKIE_SAME_SITE_NO_RESTRICTION ? "None" : "Lax") + "}";
        return true;
    }
    ~Cookies() override { text(Op::Cookies,id_,"[" + json_ + "]"); }
private:
    uint64_t id_; std::string json_; IMPLEMENT_REFCOUNTING(Cookies);
};
class DeleteCookies final : public CefCookieVisitor {
    bool Visit(const CefCookie &,int,int,bool &remove) override {remove=true;return true;}
    IMPLEMENT_REFCOUNTING(DeleteCookies);
};
CefMouseEvent mouse(Reader &r) {CefMouseEvent m;m.x=r.integer();m.y=r.integer();m.modifiers=r.value<uint32_t>();return m;}
void requestStop() {
    if(stopping.exchange(true)) return;
    navigation_condition.notify_all();
    post([] {
        for(auto &[id,s]:views) {s->closed=true;if(s->browser)s->browser->GetHost()->CloseBrowser(true);}
        exit_condition.notify_all();
    });
}
void command(Packet p) {
    Reader r{p.data};
    if(p.op==Op::Create) {
        if(stopping) return;
        auto s=std::make_shared<View>();s->id=p.view;
        auto url=r.text();s->ua=r.text();s->init=r.text();s->bridge=r.text();bool incognito=r.integer();s->js=r.integer();s->zoom=r.number();
        bool transparent=r.integer();double red=r.number(),green=r.number(),blue=r.number(),alpha=r.number();
        CefRequestContextSettings cs;
        s->context=incognito?CefRequestContext::CreateContext(cs,nullptr):CefRequestContext::GetGlobalContext();
        views[s->id]=s;live_views++;
        CefWindowInfo wi;wi.SetAsWindowless(0);
        CefBrowserSettings bs;bs.windowless_frame_rate=60;bs.javascript=s->js?STATE_ENABLED:STATE_DISABLED;
        bs.background_color=CefColorSetARGB(transparent?int(alpha*255):255,int(red*255),int(green*255),int(blue*255));
        auto extra=CefDictionaryValue::Create();
        std::string script=s->bridge+";"+s->init;
        if(!s->ua.empty()) script="Object.defineProperty(navigator,'userAgent',{get:()=>"+quote(s->ua)+"});"+script;
        extra->SetString("bridge_script",script);
        if(!CefBrowserHost::CreateBrowser(wi,new Client(s),url.empty()?"about:blank":url,bs,extra,s->context)) {
            views.erase(s->id);live_views--;text(Op::Error,s->id,"browser creation failed");output(Op::Closed,s->id);exit_condition.notify_all();
        }
        return;
    }
    auto i=views.find(p.view);if(i==views.end())return;auto s=i->second;
    if(p.op==Op::Close){s->closed=true;if(s->browser){s->browser->GetHost()->CloseDevTools();s->browser->GetHost()->CloseBrowser(true);}return;}
    if(!s->browser || ((p.op==Op::Eval || p.op==Op::Script) && !s->document_ready)) {s->pending.push_back(std::move(p));return;}
    auto b=s->browser;auto frame=b->GetMainFrame();auto focused=b->GetFocusedFrame();if(!focused)focused=frame;
    switch(p.op) {
        case Op::Resize:s->width=std::max(1,r.integer());s->height=std::max(1,r.integer());b->GetHost()->WasResized();break;
        case Op::Navigate:{auto url=r.text();{std::lock_guard lock(s->header_mutex);s->header_url=url;s->headers.clear();auto n=r.value<uint32_t>();if(n>4096)throw std::runtime_error("too many headers");while(n--){auto k=r.text();auto v=r.text();s->headers.insert({k,v});}}frame->LoadURL(url);break;}
        case Op::Back:b->GoBack();break;case Op::Forward:b->GoForward();break;case Op::Reload:b->Reload();break;case Op::StopLoad:b->StopLoad();break;
        case Op::Eval:{auto code=r.text();if(!s->js){text(Op::JsResult,s->id,"");break;}auto id=++s->next_eval;s->evaluations[id]=true;
            std::string script="(function(){var r;try{r=String(eval(decodeURIComponent(escape(atob('"+b64(code)+"')))));}catch(e){r='';}console.log('__compose_cef_eval:"+std::to_string(id)+":'+r);})();";
            frame->ExecuteJavaScript(script,frame->GetURL(),0);break;}
        case Op::Script:{auto code=r.text();focused->ExecuteJavaScript(code,focused->GetURL(),0);break;}
        case Op::Focus:b->GetHost()->SetFocus(r.integer());break;
        case Op::MouseMove:{auto m=mouse(r);b->GetHost()->SendMouseMoveEvent(m,r.integer());break;}
        case Op::MouseClick:{auto m=mouse(r);auto button=static_cast<CefBrowserHost::MouseButtonType>(r.integer());bool up=r.integer();int count=r.integer();b->GetHost()->SendMouseClickEvent(m,button,up,count);break;}
        case Op::Wheel:{auto m=mouse(r);int x=r.integer(),y=r.integer();b->GetHost()->SendMouseWheelEvent(m,x,y);break;}
        case Op::Key:{CefKeyEvent k;k.type=static_cast<cef_key_event_type_t>(r.integer());k.modifiers=r.value<uint32_t>();k.windows_key_code=r.integer();k.native_key_code=r.integer();k.is_system_key=r.integer();k.character=r.integer();k.unmodified_character=r.integer();b->GetHost()->SendKeyEvent(k);break;}
        case Op::Edit:edit(s,r.integer());break;
        case Op::Zoom:{auto z=r.number();b->GetHost()->SetZoomLevel(z==1?0:z);break;}
        case Op::OpenTools:{CefWindowInfo window;CefBrowserSettings settings;b->GetHost()->ShowDevTools(window,nullptr,settings,CefPoint(0,0));break;}
        case Op::CloseTools:b->GetHost()->CloseDevTools();break;
        case Op::MenuDone:{auto token=r.value<uint64_t>();int cmd=r.integer();auto paste=r.text();if(token==s->menu_token && s->menu_callback){s->menu_callback->Cancel();s->menu_callback=nullptr;if(cmd>=0)edit(s,cmd,paste,s->menu_frame);s->menu_frame=nullptr;}break;}
        case Op::GetCookies:s->context->GetCookieManager(nullptr)->VisitUrlCookies(r.text(),true,new Cookies(s->id));break;
        case Op::ClearCookies:s->context->GetCookieManager(nullptr)->DeleteCookies("","",nullptr);break;
        case Op::RemoveCookies:s->context->GetCookieManager(nullptr)->VisitUrlCookies(r.text(),true,new DeleteCookies());break;
        case Op::SetCookie:{CefCookie c;CefString(&c.name)=r.text();CefString(&c.value)=r.text();auto domain=r.text();CefString(&c.domain)=domain;CefString(&c.path)=r.text();c.secure=r.integer();c.httponly=r.integer();auto expires=r.value<int64_t>();auto same=r.text();
            c.has_expires=expires>0;c.expires.val=11644473600000000LL+expires*1000;c.same_site=same=="Strict"?CEF_COOKIE_SAME_SITE_STRICT_MODE:same=="None"?CEF_COOKIE_SAME_SITE_NO_RESTRICTION:CEF_COOKIE_SAME_SITE_LAX_MODE;
            s->context->GetCookieManager(nullptr)->SetCookie((c.secure?"https://":"http://")+domain,c,nullptr);break;}
        default:break;
    }
}
class App final : public CefApp {
    void OnBeforeCommandLineProcessing(const CefString &, CefRefPtr<CefCommandLine> command) override {
        command->AppendSwitch("disable-gpu");command->AppendSwitchWithValue("disable-features","Vulkan");
    }
    IMPLEMENT_REFCOUNTING(App);
};
}

int compose_cef_worker_main(int fd, const std::string &runtime, const std::string &cache) {
    channel=fd;fcntl(fd,F_SETFD,FD_CLOEXEC); // Renderer/utility children must not inherit the host channel.
    g_mkdir_with_parents(cache.c_str(),0700);
    g_mkdir_with_parents((runtime+"/locales").c_str(),0755);
    if(!g_file_test((runtime+"/locales/en-US.pak").c_str(),G_FILE_TEST_EXISTS)) {
        gchar *data=nullptr;gsize size=0;
        if(g_file_get_contents((runtime+"/en-US.pak").c_str(),&data,&size,nullptr)) {g_file_set_contents((runtime+"/locales/en-US.pak").c_str(),data,size,nullptr);g_free(data);}
    }
    char name[]="compose-cef-host";char *args[]={name,nullptr};CefMainArgs main_args(1,args);
    CefSettings settings;settings.no_sandbox=true;settings.multi_threaded_message_loop=true;settings.windowless_rendering_enabled=true;
    CefString(&settings.browser_subprocess_path)=runtime+"/cef_subprocess";CefString(&settings.resources_dir_path)=runtime;
    CefString(&settings.locales_dir_path)=runtime+"/locales";CefString(&settings.root_cache_path)=cache;CefString(&settings.cache_path)=cache+"/profile";
    CefString(&settings.log_file)=cache+"/cef.log";
    auto app=CefRefPtr<CefApp>(new App());
    bool initialized=CefInitialize(main_args,settings,app,nullptr);Writer ready;ready.integer(initialized);output(Op::Ready,0,ready);
    if(!initialized){close(fd);return 1;}
    std::thread input([] {
        Packet p;
        try {
            while(receivePacket(channel,p)) {
                if(p.op==Op::Stop){requestStop();break;}
                if(p.op==Op::NavigateReply){Reader r{p.data};auto token=r.value<uint64_t>();bool allow=r.integer();std::lock_guard lock(navigation_mutex);navigation_replies[token]=allow;navigation_condition.notify_all();continue;}
                post([p=std::move(p)]()mutable{try{command(std::move(p));}catch(const std::exception &e){text(Op::Error,0,e.what());}});
            }
        } catch(const std::exception &) {}
        requestStop();
    });
    {
        std::unique_lock lock(exit_mutex);exit_condition.wait(lock,[]{return stopping && live_views==0;});
    }
    CefShutdown(); // Exactly once, on the same main thread that initialized CEF.
    ::shutdown(fd,SHUT_RDWR);input.join();close(fd);return 0;
}
