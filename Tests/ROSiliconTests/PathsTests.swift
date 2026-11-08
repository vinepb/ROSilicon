import Foundation
import Testing
@testable import ROSilicon

@Suite(.timeLimit(.minutes(1)))
struct PathsTests {

    // MARK: - Layout

    @Test func everythingInstalledHangsOffTheRoot() {
        let paths = Paths(root: URL(filePath: "/test/root"))
        #expect(paths.prefix.path == "/test/root/wine")
        #expect(paths.driveC.path == "/test/root/wine/drive_c")
        #expect(paths.gameDir.path == "/test/root/wine/drive_c/Gravity/Ragnarok")
        #expect(paths.ragexe.path == "/test/root/wine/drive_c/Gravity/Ragnarok/Ragexe.exe")
        #expect(paths.downloads.path == "/test/root/downloads")
    }

    /// DXVK belongs in the prefix's system folder, not beside Ragexe.exe: a
    /// reinstall of the client rewrites the game folder, and Windows searches
    /// it first, so the old location would outrank the new one.
    @Test func dxvkIsLinkedIntoTheSystemFolderNotTheGameFolder() {
        let paths = Paths(root: URL(filePath: "/test/root"))
        #expect(paths.syswow64.path == "/test/root/wine/drive_c/windows/syswow64")
        #expect(paths.dxvkLink.path == "/test/root/wine/drive_c/windows/syswow64/d3d9.dll")
        #expect(paths.legacyDXVKLink.path
            == "/test/root/wine/drive_c/Gravity/Ragnarok/d3d9.dll")
        #expect(paths.dxvkLink != paths.legacyDXVKLink)
    }

    /// Wine runs from inside the app bundle, so the launcher can live anywhere
    /// and nothing under the install folder is Wine's.
    @Test func wineIsReadFromTheBundleNotTheInstallFolder() {
        let paths = Paths(root: URL(filePath: "/test/root"))
        let bundled = Paths.bundledWineRoot.path
        #expect(paths.wineRoot.path == bundled)
        #expect(paths.wine.path == bundled + "/bin/wine")
        #expect(paths.wineserver.path == bundled + "/bin/wineserver")
        #expect(paths.wineExternalLibs.path == bundled + "/lib/external")
        #expect(paths.wineTool("winecfg").path == bundled + "/bin/winecfg")
        #expect(paths.wineTool("wineconsole").path == bundled + "/bin/wineconsole")
        #expect(!paths.wine.path.hasPrefix(paths.root.path))
    }

    @Test func bundledWineRootSitsBesideTheOtherBundledTools() {
        #expect(Paths.bundledWineRoot.path == Paths.bundledTools.path + "/Wine")
        #expect(Paths.dxvkDLL.path == Paths.bundledTools.path + "/d3d9.dll")
        #expect(Paths.steamStub.path == Paths.bundledTools.path + "/steam_stub.exe")
        #expect(Paths.rosettaX87JITFolder.path == Paths.bundledTools.path + "/rosettax87_jit")
        #expect(X87Backend.rosettaX87JIT.bundledLocation == Paths.rosettaX87JITFolder)
    }

    @Test func theDefaultClientURLIsAnHTTPSTarball() {
        #expect(Paths.defaultClientURL.scheme == "https")
        #expect(Paths.defaultClientURL.pathExtension == "tar")
    }

    // MARK: - Prefix readiness

    /// The folder existing is not proof the prefix was booted: any wine call
    /// with WINEPREFIX set creates it, and only a finished bootstrap leaves
    /// both of these behind.
    @Test func prefixCountsAsInitializedOnlyOnceBootstrapFinished() throws {
        let temp = try TemporaryDirectory()
        let paths = Paths(root: temp.url)
        #expect(!paths.prefixInitialized)

        try temp.makeDirectory("wine")
        #expect(!paths.prefixInitialized)

        try temp.write(to: "wine/system.reg")
        #expect(!paths.prefixInitialized)

        try temp.makeDirectory("wine/drive_c/windows/system32")
        #expect(!paths.prefixInitialized)

        try temp.write(to: "wine/drive_c/windows/syswow64/kernel32.dll")
        #expect(paths.prefixInitialized)
    }

    /// wineboot fills system32 seconds before it starts on syswow64, so a
    /// bootstrap killed in between leaves a prefix with a whole 64-bit Windows
    /// and no 32-bit one. The client is 32-bit: calling that ready is what sent
    /// people to "could not load kernel32.dll, status c0000135".
    @Test func aPrefixWithNo32BitWindowsIsNotReady() throws {
        let temp = try TemporaryDirectory()
        let paths = Paths(root: temp.url)
        try temp.write(to: "wine/system.reg")
        try temp.makeDirectory("wine/drive_c/windows/system32")
        try temp.makeDirectory("wine/drive_c/windows/syswow64")
        #expect(!paths.prefixInitialized, "an empty syswow64 is not a booted prefix")

        // What `prepare()` leaves behind on its own: the folder and the link,
        // neither of which is Wine's doing.
        try temp.write(to: "wine/drive_c/windows/syswow64/d3d9.dll")
        #expect(!paths.prefixInitialized)
    }

    @Test func aPrefixWithSystem32ButNoRegistryIsIncomplete() throws {
        let temp = try TemporaryDirectory()
        try temp.makeDirectory("wine/drive_c/windows/system32")
        #expect(!Paths(root: temp.url).prefixInitialized)
    }

    // MARK: - The install folder

    @Test func createRootIsSafeToRepeatAndReturnsTheFolder() throws {
        let temp = try TemporaryDirectory()
        let root = temp.url.appending(path: "nested/install")
        let paths = Paths(root: root)

        #expect(try paths.createRoot() == root)
        #expect(FileManager.default.fileExists(atPath: root.path))
        try temp.write(Data("keep".utf8), to: "nested/install/marker")

        #expect(try paths.createRoot() == root)
        #expect(FileManager.default.fileExists(atPath: root.appending(path: "marker").path))
    }

    @Test func qaRootCreatesItsWineHomeInsideTheSameRemovableTree() throws {
        let temp = try TemporaryDirectory()
        let root = temp.url.appending(path: "ROSilicon QA")
        let paths = Paths(root: root, isolatesWineHome: true)

        #expect(paths.isolatedWineHome == root.appending(path: "host-home"))
        #expect(try paths.createRoot() == root)
        #expect(FileManager.default.fileExists(
            atPath: root.appending(path: "host-home").path))
    }

    @Test func bundleDataDirectoryNameCannotEscapeApplicationSupport() {
        let base = URL(filePath: "/Users/test/Library/Application Support")
        #expect(Paths.defaultInstallRoot(
            applicationSupport: base, configuredName: "ROSilicon QA").path
            == "/Users/test/Library/Application Support/ROSilicon QA")

        for invalid in [nil, "", ".", "..", "../Elsewhere", "nested/name",
                        #"nested\name"#, "disk:name"] as [String?] {
            #expect(Paths.defaultInstallRoot(
                applicationSupport: base, configuredName: invalid).path
                == "/Users/test/Library/Application Support/ROSilicon")
        }
    }

    // MARK: - The Wine environment

    @Test func wineEnvironmentPointsEveryWineVariableAtThisInstall() {
        let paths = Paths(root: URL(filePath: "/test/root"))
        let environment = paths.wineEnvironment()
        #expect(environment["WINEPREFIX"] == paths.prefix.path)
        #expect(environment["WINELOADER"] == paths.wine.path)
        #expect(environment["WINESERVER"] == paths.wineserver.path)
    }

    @Test func qaWineEnvironmentOverridesHomeWithoutChangingTheProcess() {
        let paths = Paths(
            root: URL(filePath: "/test/ROSilicon QA"), isolatesWineHome: true)
        let environment = paths.wineEnvironment()
        #expect(environment["HOME"] == "/test/ROSilicon QA/host-home")
        #expect(ProcessInfo.processInfo.environment["HOME"] != environment["HOME"])
    }

    /// The bundle's own libraries go first, and anything the process inherited
    /// is kept behind them rather than thrown away.
    @Test func wineEnvironmentPrependsTheBundledLibrariesToWhatWasInherited() {
        let paths = Paths(root: URL(filePath: "/test/root"))
        let inherited = ProcessInfo.processInfo.environment["DYLD_LIBRARY_PATH"]
        let expected = paths.wineExternalLibs.path + (inherited.map { ":\($0)" } ?? "")
        #expect(paths.wineEnvironment()["DYLD_LIBRARY_PATH"] == expected)
    }

    @Test func wineEnvironmentPutsWinesBinFirstOnPath() {
        let paths = Paths(root: URL(filePath: "/test/root"))
        let inherited = ProcessInfo.processInfo.environment["PATH"] ?? "/usr/bin:/bin"
        let path = paths.wineEnvironment()["PATH"]
        #expect(path == paths.wineRoot.appending(path: "bin").path + ":" + inherited)
    }

    /// The launcher's own variables are added to the process environment, not
    /// substituted for it, so the child keeps HOME and the rest.
    @Test func wineEnvironmentInheritsTheRestOfTheProcessEnvironment() {
        let environment = Paths(root: URL(filePath: "/test/root")).wineEnvironment()
        for (name, value) in ProcessInfo.processInfo.environment
        where !["WINEPREFIX", "WINELOADER", "WINESERVER", "X87_SIDECAR_PATH",
                "ROSETTA_X87_PATH", "DYLD_LIBRARY_PATH", "PATH"].contains(name) {
            #expect(environment[name] == value)
        }
    }

    // MARK: - The x87 hook

    /// x87sidecar is the default, and it comes alone: Wine's loader would
    /// otherwise be left to pick between two.
    @Test(.enabled(if: X87Backend.isSupportedHere, "no hook runs before macOS 26"))
    func theDefaultX87HookIsTheSidecarAlone() {
        let environment = Paths(root: URL(filePath: "/test/root")).wineEnvironment()
        #expect(environment["X87_SIDECAR_PATH"] == Paths.x87Sidecar?.path)
        #expect(environment["ROSETTA_X87_PATH"] == nil)
    }

    /// The loader tries X87_SIDECAR_PATH first, so choosing rosettax87_jit has
    /// to clear it or the choice would do nothing.
    @Test(.enabled(if: X87Backend.isSupportedHere, "no hook runs before macOS 26"))
    func choosingRosettaX87JITClearsTheSidecar() {
        let environment = Paths(root: URL(filePath: "/test/root"))
            .wineEnvironment(x87: .rosettaX87JIT)
        #expect(environment["X87_SIDECAR_PATH"] == nil)
        #expect(environment["ROSETTA_X87_PATH"] == Paths.rosettaX87JIT?.path)
    }

    /// Disabled means neither variable, so Wine's loader execs the client
    /// under stock Rosetta.
    @Test func disablingTheX87HookSetsNeither() {
        let environment = Paths(root: URL(filePath: "/test/root"))
            .wineEnvironment(x87: .disabled)
        #expect(environment["X87_SIDECAR_PATH"] == nil)
        #expect(environment["ROSETTA_X87_PATH"] == nil)
        #expect(X87Backend.disabled.executable == nil)
        #expect(X87Backend.disabled.bundledLocation == nil)
    }

    // MARK: - Macs too old for a hook

    /// Both hooks patch Rosetta's x87 translation from outside the process,
    /// and they are built against the Rosetta macOS 26 ships. Nothing older
    /// runs one. Asked as a pure question, so the answer does not depend on
    /// the Mac the tests happen to be running on.
    @Test func theHooksNeedMacOS26() {
        #expect(!X87Backend.isSupported(onMacOS: .init(
            majorVersion: 14, minorVersion: 7, patchVersion: 1)))
        #expect(!X87Backend.isSupported(onMacOS: .init(
            majorVersion: 15, minorVersion: 0, patchVersion: 0)))
        #expect(!X87Backend.isSupported(onMacOS: .init(
            majorVersion: 25, minorVersion: 9, patchVersion: 9)))
        #expect(X87Backend.isSupported(onMacOS: .init(
            majorVersion: 26, minorVersion: 0, patchVersion: 0)))
        #expect(X87Backend.isSupported(onMacOS: .init(
            majorVersion: 27, minorVersion: 1, patchVersion: 0)))
    }

    /// A preference set on a newer Mac travels with the launcher — in the
    /// same home folder, restored onto an older machine. It must not be able
    /// to put a hook back where none can run.
    @Test(.disabled(if: X87Backend.isSupportedHere, "this Mac can run a hook"))
    func nothingIsHookedOnAMacOlderThanMacOS26() {
        let paths = Paths(root: URL(filePath: "/test/root"))
        for backend in X87Backend.allCases {
            #expect(backend.onThisMac == .disabled)
            let environment = paths.wineEnvironment(x87: backend)
            #expect(environment["X87_SIDECAR_PATH"] == nil)
            #expect(environment["ROSETTA_X87_PATH"] == nil)
        }
    }

    /// The loader reads libRuntimeRosettax87 from its own folder, so on its
    /// own it is no hook.
    @Test func rosettaX87JITNeedsItsRuntimeBesideTheLoader() throws {
        let temp = try TemporaryDirectory()
        let loader = try temp.write(to: "runtime_loader")
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o755], ofItemAtPath: loader.path)
        #expect(Paths.rosettaX87JIT(in: temp.url) == nil)

        try temp.write(to: "libRuntimeRosettax87")
        #expect(Paths.rosettaX87JIT(in: temp.url) == loader)
    }

    @Test func aRosettaX87JITLoaderThatCannotRunIsNoHook() throws {
        let temp = try TemporaryDirectory()
        try temp.write(to: "runtime_loader")
        try temp.write(to: "libRuntimeRosettax87")
        #expect(Paths.rosettaX87JIT(in: temp.url) == nil)
    }

    /// The raw values are what the preferences remember, so renaming a case
    /// would quietly send someone back to the default.
    @Test func x87HooksKeepTheNamesThePreferencesStore() {
        #expect(X87Backend.default == .sidecar)
        #expect(X87Backend.allCases.map(\.rawValue)
                == ["x87sidecar", "rosettax87_jit", "disabled"])
        #expect(X87Backend.sidecar.environmentKey == "X87_SIDECAR_PATH")
        #expect(X87Backend.rosettaX87JIT.environmentKey == "ROSETTA_X87_PATH")
        #expect(X87Backend.disabled.environmentKey == nil)
        #expect(X87Backend.environmentKeys == ["X87_SIDECAR_PATH", "ROSETTA_X87_PATH"])
    }

    // MARK: - Errors

    /// The message itself is checked against the shipped strings file in
    /// `LocalizationTests`; outside the app bundle there is no strings table
    /// to look one up in, so all this can say is that a description exists.
    @Test func aMissingBundledToolDescribesItself() throws {
        let error = BundledToolsError.missing("d3d9.dll", URL(filePath: "/test/Resources"))
        #expect(!(try #require(error.errorDescription)).isEmpty)
    }
}
