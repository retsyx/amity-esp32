// Decode the browser-rendered QR with macOS Vision, without printing its secret.
import Foundation
import Vision

let image = URL(fileURLWithPath: CommandLine.arguments[1])
let expectedFile = URL(fileURLWithPath: CommandLine.arguments[2])
let expected = try JSONSerialization.jsonObject(with: Data(contentsOf: expectedFile)) as! [String: Any]
let request = VNDetectBarcodesRequest()
request.symbologies = [.qr]
try VNImageRequestHandler(url: image).perform([request])
guard request.results?.contains(where: { $0.payloadStringValue == expected["qr_payload"] as? String }) == true else {
    fputs("Rendered QR does not decode to the expected Matter payload\n", stderr)
    exit(1)
}
print("PASS rendered QR decodes to the device's Matter setup payload")
