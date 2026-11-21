import Foundation
import UserNotifications

/// Reminders before a device's install stops opening (its development profile expires, after up to a
/// year): 14 and 3 days ahead. Local notifications, so they come even while the installer is closed;
/// clicking one opens the installer.
enum Reminders {
    static let leadDays = [14, 3]

    static func schedule(for install: Install) async {
        guard install.platform != .macos, let expires = install.expires else {
            return
        }
        let center = UNUserNotificationCenter.current()
        cancel(for: install)
        guard (try? await center.requestAuthorization(options: [.alert, .sound])) == true else {
            return
        }
        let day = DateFormatter.localizedString(from: expires, dateStyle: .medium, timeStyle: .none)
        for days in leadDays {
            guard let when = Calendar.current.date(byAdding: .day, value: -days, to: expires), when > .now else {
                continue
            }
            let content = UNMutableNotificationContent()
            content.title = "Reinstall Frontline on \(install.deviceName)"
            content.body = "It stops opening on \(day). Open Frontline Installer and press Reinstall."
            content.userInfo = ["install": install.id]
            let parts = Calendar.current.dateComponents([.year, .month, .day, .hour, .minute], from: when)
            let request = UNNotificationRequest(identifier: identifier(install, days), content: content,
                                                trigger: UNCalendarNotificationTrigger(dateMatching: parts, repeats: false))
            try? await center.add(request)
        }
    }

    static func cancel(for install: Install) {
        UNUserNotificationCenter.current()
            .removePendingNotificationRequests(withIdentifiers: leadDays.map { identifier(install, $0) })
    }

    private static func identifier(_ install: Install, _ days: Int) -> String {
        "expiry.\(install.id).\(days)"
    }
}
