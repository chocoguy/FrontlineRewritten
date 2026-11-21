import Foundation

/// The apps' settings: user defaults keys, their defaults and the choices
/// offered. Each app adds its screen's `screenRate` and `screenAspect`.
enum Settings {
    /// Mouse look speed, a multiple of `Input.radiansPerCount` (default 1).
    static let mouseSensitivity = "mouseSensitivity"
    /// Moving the mouse up looks down.
    static let invertMouse = "invertMouse"
    /// Rewritten's field of view: degrees across at 4:3 (70, the original, to
    /// 110). Applies while the game runs.
    static let fieldOfView = "fieldOfView"
    static let fieldOfViews: ClosedRange<Double> = 70...110

    static func fieldOfView(_ defaults: UserDefaults) -> Double {
        let degrees = defaults.object(forKey: fieldOfView) == nil ? 70 : defaults.double(forKey: fieldOfView)
        return min(max(degrees, fieldOfViews.lowerBound), fieldOfViews.upperBound)
    }

    /// Degrees across at 4:3 widened to the width a level renders at (Hor+).
    static func widenedFieldOfView(_ degrees: Double, width: Int) -> Double {
        let half = degrees / 2 * .pi / 180
        return 2 * atan(tan(half) * Double(width) / 640) * 180 / .pi
    }

    /// Rewritten's Full Auto Springfield: it fires for as long as the trigger
    /// is held. Applies while the game runs.
    static let fullAutoSpringfield = "fullAutoSpringfield"

    /// Rewritten's mods: the game's catalog (platform/src/mods.cpp), each on
    /// or off under `defaultsKey`. They apply from the next level started.
    struct Mod: Identifiable {
        let index: Int32
        let key: String
        let title: String
        let summary: String

        var id: String { key }
        var defaultsKey: String { "mod." + key }
    }

    static let mods: [Mod] = (0..<FrontlineModCount()).map { i in
        Mod(index: i, key: String(cString: FrontlineModKey(i)), title: String(cString: FrontlineModTitle(i)),
            summary: String(cString: FrontlineModSummary(i)))
    }

    /// Pause the game when the app goes to the background (on if unset).
    static let pauseInBackground = "pauseInBackground"

    static var pausesInBackground: Bool {
        UserDefaults.standard.object(forKey: pauseInBackground) == nil
            || UserDefaults.standard.bool(forKey: pauseInBackground)
    }

    /// The share of the screen the picture fills, for TVs that crop its edges
    /// (1 if unset). A platform setting, so Classic has it too.
    static let pictureSize = "pictureSize"
    static let pictureSizes: [(share: Double, name: String)] = [
        (1, "Full screen"), (0.95, "95%"), (0.9, "90%"),
    ]

    static func pictureSize(_ defaults: UserDefaults) -> Double {
        let share = defaults.object(forKey: pictureSize) == nil ? 1 : defaults.double(forKey: pictureSize)
        return pictureSizes.contains { $0.share == share } ? share : 1
    }

    /// iPhone and iPad's touch controls: "auto" (the default: hidden while a
    /// gamepad is played), "always" or "off".
    static let touchControls = "touchControls"
    static let touchAuto = "auto"
    static let touchAlways = "always"
    static let touchOff = "off"

    static func touchControls(_ defaults: UserDefaults) -> String {
        let mode = defaults.string(forKey: touchControls) ?? touchAuto
        return [touchAuto, touchAlways, touchOff].contains(mode) ? mode : touchAuto
    }

    /// How opaque the touch controls are drawn (0.6 if unset).
    static let touchOpacity = "touchOpacity"
    static let touchOpacities: [(value: Double, name: String)] = [
        (0.3, "Faint"), (0.6, "Normal"), (0.9, "Strong"),
    ]

    static func touchOpacity(_ defaults: UserDefaults) -> Double {
        defaults.object(forKey: touchOpacity) == nil ? 0.6 : defaults.double(forKey: touchOpacity)
    }

    /// Touch look speed, a multiple of TouchControls' (1 if unset).
    static let touchLookSensitivity = "touchLookSensitivity"

    static func touchLookSensitivity(_ defaults: UserDefaults) -> Double {
        let value = defaults.object(forKey: touchLookSensitivity) == nil ? 1 : defaults.double(forKey: touchLookSensitivity)
        return min(max(value, 0.25), 4)
    }

    /// Dragging up looks down.
    static let touchInvertLook = "touchInvertLook"

    // The setup screen's, for the whole run.

    /// "classic" (the default) or "rewritten".
    static let gameMode = "gameMode"
    static let classic = "classic"
    static let rewritten = "rewritten"
    /// The render's height on a 480-line screen (Rewritten only).
    static let renderLines = "renderLines"
    /// Widescreen in the display's shape (Rewritten only).
    static let widescreen = "widescreen"
    /// Frames drawn between the game's, up to the screen's refresh rate
    /// (`Renderer.maxRate`; Rewritten only).
    static let highFrameRate = "highFrameRate"
    /// Samples a pixel: 1 (off) or 4 (Rewritten only; 4 if unset).
    static let msaa = "msaa"
    /// Times the level's own draw distance (Rewritten only).
    static let drawDistance = "drawDistance"
    /// The Modern renderer: the level drawn natively (Rewritten only; 60 fps).
    static let modernRenderer = "modernRenderer"
    /// Modern's corrected lighting: true directional and point lights.
    static let correctedLighting = "correctedLighting"
    /// The blob shadows under characters (Rewritten only).
    static let blobShadows = "blobShadows"
    /// Heavy smoke and spray drawn at about the original resolution
    /// (Rewritten only; on if unset).
    static let fastSmoke = "fastSmoke"
    /// Textures drawn larger than they are filtered bicubically, not
    /// bilinearly (Rewritten only; on if unset).
    static let bicubicTextures = "bicubicTextures"
    /// Creases and the ground under things darkened (Rewritten only; not
    /// with the high frame rate).
    static let ambientOcclusion = "ambientOcclusion"
    /// Gunfire and explosions light what's around them (Rewritten only).
    static let flashLights = "flashLights"
    /// Unfair AI: Hard's enemies aim perfectly, sense everything, press the
    /// attack, punch rapidly up close and have three times the health (Rewritten only).
    static let unfairAI = "unfairAI"

    /// The render resolutions offered: lines on the 480-line screen.
    static let resolutions: [(lines: Int, name: String)] = [
        (480, "Original"), (720, "720p"), (1080, "1080p"), (1440, "1440p"), (2160, "4K"),
    ]

    static func renderLines(_ defaults: UserDefaults) -> Int {
        let lines = defaults.integer(forKey: renderLines)
        return resolutions.contains { $0.lines == lines } ? lines : 480
    }

    static let defaultMsaa = 4
    static let defaultFastSmoke = true
    static let defaultBicubicTextures = true

    static func bicubicTextures(_ defaults: UserDefaults) -> Bool {
        defaults.object(forKey: bicubicTextures) == nil ? defaultBicubicTextures
                                                        : defaults.bool(forKey: bicubicTextures)
    }

    static func fastSmoke(_ defaults: UserDefaults) -> Bool {
        defaults.object(forKey: fastSmoke) == nil ? defaultFastSmoke : defaults.bool(forKey: fastSmoke)
    }

    static func msaa(_ defaults: UserDefaults) -> Int {
        defaults.object(forKey: msaa) == nil ? defaultMsaa : defaults.integer(forKey: msaa)
    }

    /// The draw distances offered: times the level's own.
    static let drawDistances: [(times: Double, name: String)] = [
        (1, "Original"), (2, "Far (2×)"), (4, "Farthest (4×)"),
    ]
    static let defaultDrawDistance = 2.0

    static func drawDistance(_ defaults: UserDefaults) -> Double {
        let times = defaults.object(forKey: drawDistance) == nil ? defaultDrawDistance
                                                                  : defaults.double(forKey: drawDistance)
        return drawDistances.contains { $0.times == times } ? times : 1
    }

    /// The width the game renders a level at for `aspect`, in the console's
    /// pixels (as NativeRenderWidth works it out).
    static func renderWidth(aspect: CGFloat?) -> Int {
        guard let aspect, aspect > 4.0 / 3.0 else {
            return 640
        }
        return min(1136, max(640, Int((480 * aspect / 16).rounded()) * 16))
    }

    /// Hands the game the setup screen's settings, for the whole run; call
    /// before FrontlineStart. `aspect`: the screen's, which widescreen follows.
    static func apply(_ defaults: UserDefaults, aspect: CGFloat) {
        let rewritten = defaults.string(forKey: gameMode) == Settings.rewritten
        FrontlineSetGameMode(Int32(rewritten ? FrontlineModeRewritten : FrontlineModeClassic))
        FrontlineSetRenderLines(Int32(renderLines(defaults)))
        FrontlineSetWidescreen(defaults.bool(forKey: widescreen) ? Float(aspect) : 0)
        FrontlineSetHighFrameRate(defaults.bool(forKey: highFrameRate))
        FrontlineSetMsaa(Int32(msaa(defaults)))
        FrontlineSetDrawDistance(Float(drawDistance(defaults)))
        FrontlineSetModernRenderer(defaults.bool(forKey: modernRenderer))
        FrontlineSetCorrectedLighting(defaults.bool(forKey: correctedLighting))
        FrontlineSetBlobShadows(defaults.bool(forKey: blobShadows))
        FrontlineSetFastSmoke(fastSmoke(defaults))
        FrontlineSetBicubicTextures(bicubicTextures(defaults))
        FrontlineSetAmbientOcclusion(defaults.bool(forKey: ambientOcclusion))
        FrontlineSetFlashLights(defaults.bool(forKey: flashLights))
        FrontlineSetUnfairAI(defaults.bool(forKey: unfairAI))
        FrontlineSetPictureSize(Float(pictureSize(defaults)))
    }

    /// Hands the game the settings that apply while it runs; call every frame.
    static func applyLive(_ defaults: UserDefaults) {
        FrontlineSetFieldOfView(Float(fieldOfView(defaults)))
        FrontlineSetFullAutoSpringfield(defaults.bool(forKey: fullAutoSpringfield))
        for mod in mods {
            FrontlineSetModOn(mod.index, defaults.bool(forKey: mod.defaultsKey))
        }
    }
}
