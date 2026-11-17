// "航班牌" 桌面应用: 一个窗口, 里面显示后台推送程序提供的控制页面 (http://127.0.0.1:8377)。
// 真正干活的是开机自启的后台程序(host.py); 这个应用只是它的界面, 关掉窗口不影响屏幕更新。
import Cocoa
import WebKit

let pageURL = URL(string: "http://127.0.0.1:8377/")!

// 网页铺满了整个窗口(包括标题栏那一条), 鼠标事件都被网页吃掉, 窗口就拖不动了。
// 在顶上盖一条透明的视图专门负责拖动窗口; 红绿灯按钮在它上面, 不受影响。
class DragStrip: NSView {
    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 2 { window?.zoom(nil) } else { window?.performDrag(with: event) }
    }
}

class AppDelegate: NSObject, NSApplicationDelegate, WKNavigationDelegate {
    var window: NSWindow!
    var web: WKWebView!

    func applicationDidFinishLaunching(_ note: Notification) {
        let menu = NSMenu()
        let appItem = NSMenuItem()
        menu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "重新载入", action: #selector(reload), keyEquivalent: "r")
        appMenu.addItem(NSMenuItem.separator())
        appMenu.addItem(withTitle: "退出航班牌", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        // 编辑菜单: 没有它搜索框里不能用 ⌘C / ⌘V / ⌘A
        let editItem = NSMenuItem()
        menu.addItem(editItem)
        let edit = NSMenu(title: "编辑")
        edit.addItem(withTitle: "剪切", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = edit
        NSApp.mainMenu = menu

        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 440, height: 640),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                          backing: .buffered, defer: false)
        window.title = "航班牌"
        window.titleVisibility = .hidden            // 标题栏和页面融成一体, 页面自己顶上留了空
        window.titlebarAppearsTransparent = true
        window.minSize = NSSize(width: 340, height: 420)
        window.center()
        window.setFrameAutosaveName("FlightBoardWindow")

        web = WKWebView(frame: window.contentView!.bounds)
        web.autoresizingMask = [.width, .height]
        web.navigationDelegate = self
        web.setValue(false, forKey: "drawsBackground")
        window.contentView!.addSubview(web)
        let b = window.contentView!.bounds
        let strip = DragStrip(frame: NSRect(x: 0, y: b.height - 40, width: b.width, height: 40))
        strip.autoresizingMask = [.width, .minYMargin]
        window.contentView!.addSubview(strip)
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        reload()
    }

    @objc func reload() {
        web.load(URLRequest(url: pageURL, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 5))
    }

    // 后台程序没在运行时页面打不开: 给个说明, 每 3 秒自动再试
    func webView(_ w: WKWebView, didFailProvisionalNavigation nav: WKNavigation!, withError error: Error) {
        let html = """
        <meta name="color-scheme" content="light dark">
        <body style="font:15px -apple-system;padding:44px 24px;color:#86868b">后台没有运行</body>
        """
        w.loadHTMLString(html, baseURL: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [weak self] in self?.reload() }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
