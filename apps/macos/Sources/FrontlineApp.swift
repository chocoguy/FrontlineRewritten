import SwiftUI

@main
struct FrontlineApp: App {
    var body: some Scene {
        WindowGroup("Frontline Rewritten") {
            GameView()
                // The game renders 640×480 at 4:3; widescreen is a Phase 3 job.
                .aspectRatio(4.0 / 3.0, contentMode: .fit)
                .frame(minWidth: 640, minHeight: 480)
                .background(.black)
        }
        .windowResizability(.contentMinSize)
    }
}
