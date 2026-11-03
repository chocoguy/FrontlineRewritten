import GameController
import SwiftUI
import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // A TV is widescreen.
        UserDefaults.standard.register(defaults: [Settings.widescreen: true])
        if Saves.sync != nil {
            PadBindings.syncWithICloud()
        }
        Saves.prepare()
        return true
    }

    func application(_ application: UIApplication, configurationForConnecting session: UISceneSession,
                     options: UIScene.ConnectionOptions) -> UISceneConfiguration {
        let configuration = UISceneConfiguration(name: "Default", sessionRole: session.role)
        configuration.delegateClass = SceneDelegate.self
        return configuration
    }
}

/// The app's one window: the setup screen, then the game.
///
/// The game's view controller is the window's root, not a SwiftUI view's
/// child: as a GCEventViewController it keeps the controllers' and the
/// remote's buttons from UIKit (Menu would leave the app), which it only does
/// reliably at the root.
@objc(FrontlineSceneDelegate)
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let scene = scene as? UIWindowScene else {
            return
        }
        let window = UIWindow(windowScene: scene)
        window.rootViewController = UIHostingController(rootView: SetupView { [weak self] in
            await self?.play()
        })
        window.makeKeyAndVisible()
        self.window = window
    }

    /// Starts the game and shows it; the reason, if it can't start.
    private func play() async -> String? {
        if let problem = await Game.start() {
            return problem
        }
        window?.rootViewController = GameViewController()
        return nil
    }

    func sceneWillResignActive(_ scene: UIScene) {
        // Pause on leaving the app (the TV button, a notification): tvOS
        // suspends it in the background.
        if Game.started && FrontlineIsPlaying() {
            FrontlinePressAction(0, UInt32(FrontlineActionPause))
        }
    }
}
