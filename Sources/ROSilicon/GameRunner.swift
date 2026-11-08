import Foundation

enum RunError: LocalizedError {
    case missingWine(URL)
    case missingFile(String, URL)
    case gameNotInstalled(URL)
    case exited(Int32)

    var errorDescription: String? {
        switch self {
        case .missingWine(let url): Strings.errorMissingWine(url.path)
        case .missingFile(let what, let url): Strings.errorMissingFile(what, url.path)
        case .gameNotInstalled(let url): Strings.errorGameNotInstalled(url.path)
        case .exited(let status): Strings.errorGameExited(status)
        }
    }
}

/// Wine's own tools, offered behind ⌥ for poking at the prefix by hand.
enum WineTool: Sendable {
    case winecfg
    case commandPrompt

    /// The loader in Wine's bin/ that starts it. cmd.exe goes through
    /// wineconsole because it needs a console window drawn for it, and a
    /// launcher started from Finder has no terminal to inherit.
    var executable: String {
        switch self {
        case .winecfg: "winecfg"
        case .commandPrompt: "wineconsole"
        }
    }

    var arguments: [String] {
        switch self {
        case .winecfg: []
        case .commandPrompt: ["cmd"]
        }
    }

    /// What the log calls it, untranslated: these are program names.
    var label: String {
        switch self {
        case .winecfg: "winecfg"
        case .commandPrompt: "cmd.exe"
        }
    }
}

/// Launches the client the way Run.command does.
struct GameRunner: Sendable {
    let paths: Paths
    let reporter: Reporter
    /// Draws Metal's frame-rate overlay on top of the client. Off unless
    /// someone turned it on in the menu; quitting never needs it.
    var metalHUD = false
    /// Wine's debug channels and any extra variables set from the menu.
    /// Untouched by default, which is the launcher's own quiet environment.
    var options = LaunchOptions()
    var keyboard = GameKeyboardSettings()
    /// The x87 hook 32-bit programs run under, for the game and Wine's tools
    /// alike. x87sidecar unless someone chose otherwise in the menu.
    var x87 = X87Backend.default

    /// The hook that will actually run, which is none at all before macOS 26.
    /// What the log says and what `prepare()` insists on both follow from
    /// this rather than from the preference, so neither talks about a hook
    /// this Mac is never going to load.
    private var hook: X87Backend { x87.onThisMac }

    /// Shuts down everything in the prefix. Wine's own way of doing it, so a
    /// hung client goes down with it rather than being orphaned.
    func quit() async {
        var environment = paths.wineEnvironment()
        environment["WINEDEBUG"] = "-all"
        _ = try? await Shell.run(paths.wineserver, ["-k"], environment: environment)
    }

    /// Opens one of Wine's tools against the prefix, in a window of its own.
    ///
    /// Unlike playing, this needs neither the client nor the bundled DLLs, so
    /// it stays available on a half-finished install — which is when it is
    /// most useful. An uninitialized prefix gets bootstrapped on the way, the
    /// same as any other wine invocation would do.
    func open(_ tool: WineTool) async throws {
        guard FileManager.default.isExecutableFile(atPath: paths.wine.path) else {
            throw RunError.missingWine(paths.wine)
        }
        let executable = paths.wineTool(tool.executable)
        guard FileManager.default.isExecutableFile(atPath: executable.path) else {
            throw RunError.missingFile(tool.executable, executable)
        }

        var environment = paths.wineEnvironment(x87: x87)
        await applyOptions(to: &environment)
        await reporter.log(Strings.logOpeningTool(tool.label))
        // Nothing is logged on success: winecfg blocks until its window is
        // closed, while wineconsole returns the moment it has handed the
        // console off, so "closed" would be a lie for one of the two.
        let status = try await Shell.run(
            executable, tool.arguments, environment: environment
        ) { line in await reporter.log(line) }
        if status != 0 {
            await reporter.log(Strings.logToolExited(tool.label, status))
        }
    }

    func play() async throws {
        let steamExe = try prepare()

        var environment = Self.gameEnvironment(
            from: paths.wineEnvironment(x87: x87), x87: hook, metalHUD: metalHUD)
        // DXVK renders through MoltenVK, so the overlay Metal itself draws is
        // the one that shows the frame rate of the client.
        if metalHUD {
            await reporter.log(Strings.logMetalHUD)
        }
        // Anything but the default is said: rosettax87_jit's before the
        // password prompt appears, so it does not come out of nowhere, and no
        // hook at all so a slow game says why.
        switch hook {
        case .sidecar: break
        case .rosettaX87JIT: await reporter.log(Strings.logRosettaX87JIT)
        case .disabled:
            // Told apart, because one of the two is a choice and the other is
            // the Mac: someone who picked a hook and got none deserves to be
            // told why rather than left wondering.
            await reporter.log(X87Backend.isSupportedHere
                ? Strings.logX87Disabled
                : Strings.logX87NeedsMacOS(X87Backend.firstSupportedMacOS))
        }
        await applyOptions(to: &environment)

        try await keyboard.apply(wine: paths.wine, environment: environment, reporter: reporter)

        await reporter.step(Strings.stepRunning)
        await reporter.log(Strings.logLaunching)
        // The client is started through steam.exe because it expects a Steam
        // process to be present.
        let status = try await Shell.run(
            paths.wine, [steamExe.path], environment: environment
        ) { line in await reporter.log(line) }

        if Task.isCancelled { throw CancellationError() }
        guard status == 0 else { throw RunError.exited(status) }
        await reporter.log(Strings.logExitedNormally)
    }

    /// Game-only defaults, applied before the advanced options. Keeping this
    /// pure lets tests check the complete launch policy without starting Wine.
    static func gameEnvironment(
        from inherited: [String: String], x87: X87Backend, metalHUD: Bool
    ) -> [String: String] {
        var environment = inherited
        environment["WINEDLLOVERRIDES"] = "d3d9=n,b"
        environment["MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS"] = "1"
        environment["DXVK_ASYNC"] = "1"
        if metalHUD { environment["MTL_HUD_ENABLED"] = "1" }
        if x87 == .sidecar {
            // The optional decoder substitutions are separate from x87
            // acceleration. With the updated client, QA reproduced Rosetta's
            // JIT-breakpoint assertion with them on and reached login with
            // them off. Keep the normal x87 translator and graphics defaults;
            // advanced NAME=value options can still override this for testing.
            environment["X87_NO_DECODE_HOOK"] = "1"
        }
        return environment
    }

    /// Sets Wine's debug channels and the extra variables, last so they take
    /// precedence over everything the launcher chose itself. What is not the
    /// default gets logged, so a run started with unusual settings says so.
    private func applyOptions(to environment: inout [String: String]) async {
        let extras = options.apply(to: &environment)
        if options.wineDebugValue != LaunchOptions.defaultWineDebug {
            await reporter.log(Strings.logWineDebug(options.wineDebugValue))
        }
        if !extras.isEmpty {
            await reporter.log(Strings.logExtraEnvironment(extras.joined(separator: ", ")))
        }
    }

    /// Checks the pieces are in place and links DXVK and the Steam stub, both
    /// inside the app, into the prefix: DXVK into the prefix's syswow64, the
    /// stub into drive_c. The links are checked every launch, so one left
    /// pointing at an app that has since moved is replaced rather than
    /// followed. Returns the path of steam.exe inside drive_c.
    @discardableResult
    func prepare() throws -> URL {
        guard FileManager.default.isExecutableFile(atPath: paths.wine.path) else {
            throw RunError.missingWine(paths.wine)
        }
        guard FileManager.default.fileExists(atPath: Paths.dxvkDLL.path) else {
            throw RunError.missingFile("d3d9.dll", Paths.dxvkDLL)
        }
        guard FileManager.default.fileExists(atPath: Paths.steamStub.path) else {
            throw RunError.missingFile("steam_stub.exe", Paths.steamStub)
        }
        // Without it Wine would quietly run the client under stock Rosetta,
        // playable but slow — not what someone who chose a hook asked for.
        if let location = hook.bundledLocation, hook.executable == nil {
            throw RunError.missingFile(hook.rawValue, location)
        }
        guard FileManager.default.fileExists(atPath: paths.gameDir.path) else {
            throw RunError.gameNotInstalled(paths.gameDir)
        }

        let steamExe = paths.driveC.appending(path: "steam.exe")
        // A prefix booted by an older Wine, or one still being bootstrapped,
        // may not have the folder yet; the link is what has to be there.
        try FileManager.default.createDirectory(
            at: paths.syswow64, withIntermediateDirectories: true)
        try Self.link(Paths.dxvkDLL, at: paths.dxvkLink)
        // After the new link, never before: between the two there is always a
        // d3d9.dll for a client that happens to be starting.
        Self.removeStaleLink(at: paths.legacyDXVKLink)
        try Self.link(Paths.steamStub, at: steamExe)
        return steamExe
    }

    /// Clears the link older versions put beside Ragexe.exe. The game folder
    /// is searched before syswow64, so one left there would go on deciding
    /// which d3d9.dll the client loads. Only a symlink is removed: a real DLL
    /// there was put there by hand and is not the launcher's to delete.
    static func removeStaleLink(at location: URL) {
        let fm = FileManager.default
        guard (try? fm.destinationOfSymbolicLink(atPath: location.path)) != nil else { return }
        try? fm.removeItem(at: location)
    }

    /// `ln -sfn`: replace whatever is there with a symlink — unless it already
    /// is that symlink. A client started beside a running one must not find
    /// d3d9.dll missing because the second launch was busy rewriting it.
    static func link(_ target: URL, at location: URL) throws {
        let fm = FileManager.default
        if (try? fm.destinationOfSymbolicLink(atPath: location.path)) == target.path { return }
        if (try? location.checkResourceIsReachable()) == true
            || (try? fm.attributesOfItem(atPath: location.path)) != nil {
            try fm.removeItem(at: location)
        }
        try fm.createSymbolicLink(at: location, withDestinationURL: target)
    }
}
