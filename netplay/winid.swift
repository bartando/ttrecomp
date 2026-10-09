import CoreGraphics
let windows = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
var best: (Int, Double) = (0, 0)
for w in windows where (w[kCGWindowOwnerName as String] as? String) == "tabletennis" {
    let b = w[kCGWindowBounds as String] as! [String: Double]
    let area = (b["Width"] ?? 0) * (b["Height"] ?? 0)
    if (w[kCGWindowLayer as String] as? Int) == 0 && area > best.1 { best = (w[kCGWindowNumber as String] as! Int, area) }
}
print(best.0)
