import Testing
@testable import ROSilicon

@Suite(.timeLimit(.minutes(1)))
struct GameEnvironmentTests {
    @Test func sidecarKeepsAccelerationButSkipsOptionalDecoderHook() {
        let environment = GameRunner.gameEnvironment(
            from: ["X87_SIDECAR_PATH": "/test/x87sidecar", "WINEPREFIX": "/test/wine"],
            x87: .sidecar, metalHUD: false)

        #expect(environment["X87_SIDECAR_PATH"] == "/test/x87sidecar")
        #expect(environment["WINEPREFIX"] == "/test/wine")
        #expect(environment["X87_NO_DECODE_HOOK"] == "1")
        for key in ["X87_DISABLE_HOOK", "X87_ALWAYS_NONE", "X87_DISABLE_X87_IR",
                    "X87_DISABLE_CACHE", "X87_LOG_OPS"] {
            #expect(environment[key] == nil)
        }
    }

    @Test(arguments: [X87Backend.disabled, .rosettaX87JIT])
    func otherBackendsDoNotAcquireTheSidecarWorkaround(backend: X87Backend) {
        let environment = GameRunner.gameEnvironment(from: [:], x87: backend, metalHUD: false)
        #expect(environment["X87_NO_DECODE_HOOK"] == nil)
    }

    @Test func rendererDefaultsAndUnrelatedEnvironmentArePreserved() {
        let environment = GameRunner.gameEnvironment(
            from: ["HOME": "/test/home", "OTHER": "keep"], x87: .sidecar, metalHUD: false)
        #expect(environment["WINEDLLOVERRIDES"] == "d3d9=n,b")
        #expect(environment["DXVK_ASYNC"] == "1")
        #expect(environment["MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS"] == "1")
        #expect(environment["MTL_HUD_ENABLED"] == nil)
        #expect(environment["HOME"] == "/test/home")
        #expect(environment["OTHER"] == "keep")
    }

    @Test func metalHUDRemainsOptIn() {
        let environment = GameRunner.gameEnvironment(from: [:], x87: .sidecar, metalHUD: true)
        #expect(environment["MTL_HUD_ENABLED"] == "1")
        #expect(environment["X87_NO_DECODE_HOOK"] == "1")
    }

    @Test func advancedOptionsCanReenableTheDecoderForComparisons() {
        var environment = GameRunner.gameEnvironment(from: [:], x87: .sidecar, metalHUD: false)
        let options = LaunchOptions(extraEnvironment: "X87_NO_DECODE_HOOK=0;DXVK_ASYNC=0")
        options.apply(to: &environment)
        #expect(environment["X87_NO_DECODE_HOOK"] == "0")
        #expect(environment["DXVK_ASYNC"] == "0")
        #expect(environment["WINEDEBUG"] == LaunchOptions.defaultWineDebug)
    }
}
