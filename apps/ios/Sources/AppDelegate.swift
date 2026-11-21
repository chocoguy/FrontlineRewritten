import GameController
import SwiftUI
import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions options: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        // A TV, a phone and a tablet are all wider than 4:3.
        UserDefaults.standard.register(defaults: [Settings.widescreen: true])
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
/// child: on tvOS, as a GCEventViewController it keeps the controllers' and
/// the remote's buttons from UIKit (Menu would leave the app), which it only
/// does reliably at the root; on iOS it decides the home indicator and the
/// screen edges' gestures.
@objc(FrontlineSceneDelegate)
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let scene = scene as? UIWindowScene else {
            return
        }
        let window = UIWindow(windowScene: scene)
        self.window = window
        showSetup()
        window.makeKeyAndVisible()
    }

    /// The setup screen: at launch, and after Exit Game.
    private func showSetup() {
        window?.rootViewController = UIHostingController(rootView: SetupView { [weak self] in
            self?.play()
        })
    }

    /// Starts the game and shows it; the reason, if it can't start.
    private func play() -> String? {
        if let problem = Game.start() {
            return problem
        }
        let game = GameViewController()
        game.exited = { [weak self] in
            self?.showSetup()
        }
        window?.rootViewController = game
        return nil
    }

    func sceneWillResignActive(_ scene: UIScene) {
        // Pause on leaving the app (the TV button, a notification, the home
        // gesture): the system suspends it in the background. On iPhone and
        // iPad it's a setting, as on the Mac.
        #if os(tvOS)
        let pause = true
        #else
        let pause = Settings.pausesInBackground
        #endif
        if pause && Game.started && FrontlineIsPlaying() {
            FrontlinePressAction(0, UInt32(FrontlineActionPause))
        }
    }
}
