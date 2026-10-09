import CoreGraphics
// Prints "pid windowid" for each tabletennis window (largest per process).
let windows = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
var best: [Int: (Int, Double)] = [:]
for w in windows where (w[kCGWindowOwnerName as String] as? String) == "tabletennis" {
    guard (w[kCGWindowLayer as String] as? Int) == 0 else { continue }
    let pid = w[kCGWindowOwnerPID as String] as! Int
    let b = w[kCGWindowBounds as String] as! [String: Double]
    let area = (b["Width"] ?? 0) * (b["Height"] ?? 0)
    if area > (best[pid]?.1 ?? 0) { best[pid] = (w[kCGWindowNumber as String] as! Int, area) }
}
for (pid, entry) in best { print(pid, entry.0) }
