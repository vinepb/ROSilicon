import Foundation

enum BundledToolsError: LocalizedError {
    case missing(String, URL)

    var errorDescription: String? {
        switch self {
        case .missing(let name, let folder):
            Strings.errorBundledToolMissing(name, folder.path)
        }
    }
}

/// Every path and pinned version the launcher needs.
///
/// `root` is where the launcher installs, under ~/Library/Application
/// Support/ROSilicon: a prefix per profile, each with its own game, and
/// `profile` is the one every prefix path below points into. Wine
/// is not among them — it runs from inside the app bundle, which also carries
/// DXVK, x87sidecar and the Steam stub, so the launcher downloads nothing but
/// the game client and can live anywhere, /Applications included.
struct Paths: Sendable {
    static let dataDirectoryInfoKey = "ROSiliconDataDirectoryName"
    static let isolatedWineHomeInfoKey = "ROSiliconIsolatedWineHome"
    static let defaultDataDirectoryName = "ROSilicon"

    static let defaultClientURL = URL(string:
        "https://ro1patch.gnjoylatam.com/LIVE/client/LATAM_RO1_Live_20260601_091136.tar")!

    let root: URL
    /// Whose prefix `prefix`, `driveC` and the game folder are.
    let profile: Profile
    /// QA bundles keep Wine's Unix-facing home beside their prefix. This is
    /// false for production so existing installs retain their current layout.
    let isolatesWineHome: Bool

    init(root: URL, profile: Profile = .default, isolatesWineHome: Bool = false) {
        self.root = root
        self.profile = profile
        self.isolatesWineHome = isolatesWineHome
    }

    /// WineAndAqua creates the Windows user's directory under `$HOME/Wine`.
    /// For a QA bundle, put that host home under the QA root so the test
    /// installation neither reads nor leaves data in the production ~/Wine.
    var isolatedWineHome: URL? {
        isolatesWineHome ? root.appending(path: "host-home") : nil
    }

    /// Wine runs where it lies, inside the app bundle. Nothing writes to it, so
    /// the tree under the signature is never touched after the build.
    var wineRoot: URL { Self.bundledWineRoot }
    var wine: URL { wineRoot.appending(path: "bin/wine") }
    var wineserver: URL { wineRoot.appending(path: "bin/wineserver") }
    var wineExternalLibs: URL { wineRoot.appending(path: "lib/external") }

    /// One of Wine's own tools beside `wine` itself, e.g. winecfg.
    func wineTool(_ name: String) -> URL { wineRoot.appending(path: "bin/" + name) }

    /// `wine/` for the default profile, `profiles/<name>/` for the rest.
    var prefix: URL { root.appending(path: profile.folder) }
    var driveC: URL { prefix.appending(path: "drive_c") }
    var gameDir: URL { driveC.appending(path: "Gravity/Ragnarok") }
    var ragexe: URL { gameDir.appending(path: "Ragexe.exe") }

    /// Wine's folder for 32-bit system DLLs, where a 32-bit program looks for
    /// d3d9.dll once its own folder holds none.
    var syswow64: URL { driveC.appending(path: "windows/syswow64") }

    /// Where DXVK is linked into the prefix: the system folder rather than the
    /// game's, so it is not something a reinstall of the client can drop and
    /// every 32-bit program in the prefix sees the same Direct3D 9.
    var dxvkLink: URL { syswow64.appending(path: "d3d9.dll") }

    /// Where versions up to 0.2.0 put that link, beside Ragexe.exe. Windows
    /// searches the program's own folder first, so one left there would still
    /// be the d3d9.dll the client loads; `prepare()` clears it.
    var legacyDXVKLink: URL { gameDir.appending(path: "d3d9.dll") }

    /// Wine's stamp saying the prefix is as new as the runtime that booted
    /// it. wineboot compares it against wine.inf and skips the install that
    /// fills drive_c when the two agree — so a prefix that is stamped but
    /// half-filled can only be repaired by dropping this first.
    var prefixUpdateStamp: URL { prefix.appending(path: ".update-timestamp") }

    /// True when the prefix has actually been booted, not merely created.
    ///
    /// Any wine invocation with WINEPREFIX set bootstraps the prefix, so the
    /// folder existing is not proof it is complete. The 32-bit kernel32 is
    /// what makes the third check worth its cost: wineboot fills system32
    /// seconds before it starts on syswow64, and a bootstrap killed in
    /// between — a logout, a crash, a full disk — leaves a whole 64-bit
    /// Windows with nothing to run a 32-bit program under. The client is
    /// 32-bit, so such a prefix passed the first two checks, called itself
    /// ready, and died with "could not load kernel32.dll, status c0000135".
    var prefixInitialized: Bool {
        let fm = FileManager.default
        return fm.fileExists(atPath: prefix.appending(path: "system.reg").path)
            && fm.fileExists(atPath: driveC.appending(path: "windows/system32").path)
            && fm.fileExists(atPath: syswow64.appending(path: "kernel32.dll").path)
    }

    /// Where the app keeps everything it ships with: the Wine runtime, DXVK,
    /// x87sidecar and the Steam stub.
    ///
    /// Normally the app's own Resources/. `RO_TOOLS` points somewhere else when
    /// the code runs outside a bundle, as it does under `swift run`.
    static let bundledTools: URL = {
        if let override = ProcessInfo.processInfo.environment["RO_TOOLS"] {
            return URL(filePath: override).standardizedFileURL
        }
        return Bundle.main.resourceURL ?? Bundle.main.bundleURL
    }()

    /// The Wine runtime inside the app bundle, put there by `build.sh`.
    static var bundledWineRoot: URL { bundledTools.appending(path: "Wine") }

    /// The three helper binaries, read where they lie in the app. The prefix
    /// links to the two Windows ones rather than holding copies — DXVK into
    /// `syswow64`, the stub into drive_c; `prepare()` rewrites those links on
    /// every launch, so they follow the app when it moves.
    static var dxvkDLL: URL { bundledTools.appending(path: "d3d9.dll") }
    static var steamStub: URL { bundledTools.appending(path: "steam_stub.exe") }

    /// The arm64 helper Wine's loader re-execs itself under. nil when the app
    /// was built without it, which is the only way it can be absent.
    static var x87Sidecar: URL? {
        let sidecar = bundledTools.appending(path: "x87sidecar")
        return FileManager.default.isExecutableFile(atPath: sidecar.path) ? sidecar : nil
    }

    /// rosettax87_jit, the x87 hook offered instead of x87sidecar behind ⌥:
    /// its loader and the runtime it injects, which travel together.
    static var rosettaX87JITFolder: URL { bundledTools.appending(path: "rosettax87_jit") }

    /// rosettax87_jit's loader, nil when the app was built without it.
    static var rosettaX87JIT: URL? { rosettaX87JIT(in: rosettaX87JITFolder) }

    /// The loader in `folder`, provided libRuntimeRosettax87 is beside it: the
    /// loader reads the runtime from its own folder, so one without the other
    /// is no hook at all.
    static func rosettaX87JIT(in folder: URL) -> URL? {
        let fm = FileManager.default
        let loader = folder.appending(path: "runtime_loader")
        guard fm.isExecutableFile(atPath: loader.path),
              fm.fileExists(atPath: folder.appending(path: "libRuntimeRosettax87").path)
        else { return nil }
        return loader
    }

    /// Everything the app must carry for an install to be possible. Throws
    /// naming the first one missing, which means a build that left it out.
    static func verifyBundledTools() throws {
        for name in ["d3d9.dll", "steam_stub.exe", "x87sidecar"] {
            let url = bundledTools.appending(path: name)
            guard FileManager.default.fileExists(atPath: url.path) else {
                throw BundledToolsError.missing(name, bundledTools)
            }
        }
    }

    var downloads: URL { root.appending(path: "downloads") }

    /// How the bundled runtime names itself, e.g. "11.13 (r16)", read from the
    /// lock the build embeds in the tree. nil when the app was built without a
    /// runtime, or the lock cannot be read.
    static var bundledWineVersion: String? {
        let lock = bundledWineRoot.appending(path: "share/wowsilicon/runtime-lock.json")
        guard let data = try? Data(contentsOf: lock),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let wine = (json["wine"] as? [String: Any])?["version"] as? String
        else { return nil }
        guard let revision = json["runtimeRevision"] as? Int else { return wine }
        return "\(wine) (r\(revision))"
    }

    /// Environment shared by every Wine invocation — the Swift side of `wine_env`.
    /// `x87` is the hook 32-bit programs run under; only the ⌥ menu's choice
    /// for the game and Wine's tools ever asks for anything but the default.
    func wineEnvironment(x87: X87Backend = .default) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        if let isolatedWineHome { env["HOME"] = isolatedWineHome.path }
        env["WINEPREFIX"] = prefix.path
        env["WINELOADER"] = wine.path
        env["WINESERVER"] = wineserver.path
        // Wine's loader re-execs itself under `x87sidecar --cooperative` or
        // rosettax87_jit's `runtime_loader`, whichever is named here, and
        // under neither when the hook is disabled. That is what makes the
        // client's legacy x87 float code fast on Apple Silicon. The loader
        // tries X87_SIDECAR_PATH first, so both are cleared — one inherited
        // from whoever started the launcher included — and only the chosen
        // one is set.
        //
        // `onThisMac` is what keeps a Mac older than macOS 26 out of the
        // hooks. Every wine the launcher starts is given its environment
        // here — the game, wineboot, winecfg, the registry edit — so this is
        // the one place that has to hold for none of them to be hooked.
        let chosen = x87.onThisMac
        for key in X87Backend.environmentKeys { env[key] = nil }
        if let key = chosen.environmentKey, let hook = chosen.executable { env[key] = hook.path }
        // Wine dlopen()s freetype, gnutls, MoltenVK and SDL2 by leaf name; the
        // bundle keeps them here rather than relying on a system copy.
        let dyld = env["DYLD_LIBRARY_PATH"].map { ":\($0)" } ?? ""
        env["DYLD_LIBRARY_PATH"] = wineExternalLibs.path + dyld
        env["PATH"] = wineRoot.appending(path: "bin").path + ":" + (env["PATH"] ?? "/usr/bin:/bin")
        return env
    }

    /// Where the launcher installs, by default
    /// ~/Library/Application Support/ROSilicon. `RO_ROOT` overrides it.
    static func dataDirectoryName(_ configured: String?) -> String {
        guard let configured, !configured.isEmpty,
              configured != ".", configured != "..",
              !configured.contains("/"), !configured.contains(":"),
              !configured.contains("\\")
        else { return defaultDataDirectoryName }
        return configured
    }

    static func defaultInstallRoot(applicationSupport: URL, configuredName: String?) -> URL {
        applicationSupport.appending(path: dataDirectoryName(configuredName))
    }

    static var installRoot: URL {
        if let override = ProcessInfo.processInfo.environment["RO_ROOT"] {
            return URL(filePath: override).standardizedFileURL
        }
        let base = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(filePath: NSHomeDirectory()).appending(path: "Library/Application Support")
        let configured = Bundle.main.object(forInfoDictionaryKey: dataDirectoryInfoKey) as? String
        return defaultInstallRoot(applicationSupport: base, configuredName: configured)
    }

    static func locateRoot(profile: Profile = .default) -> Paths {
        let isolatesWineHome = Bundle.main.object(
            forInfoDictionaryKey: isolatedWineHomeInfoKey) as? Bool ?? false
        return Paths(root: installRoot, profile: profile, isolatesWineHome: isolatesWineHome)
    }

    /// Creates the install folder. Called before installing and before showing
    /// the folder in Finder, so neither ever faces a missing directory.
    @discardableResult
    func createRoot() throws -> URL {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        if let isolatedWineHome {
            try FileManager.default.createDirectory(
                at: isolatedWineHome, withIntermediateDirectories: true)
        }
        return root
    }

}
