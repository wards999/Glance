import Foundation
import ImageIO
import UniformTypeIdentifiers

/// Text for the info overlay (F2).
enum InfoFormatter {
    static func text(for info: ImageInfo, position: String, size: CGSize, zoom: CGFloat, page: Int, edited: Bool) -> String {
        var lines = [info.url.lastPathComponent, position]

        if let pdf = info.pdf {
            lines.append(pdf.pagesText)
            if let title = pdf.title { lines.append("Title   " + title) }
            if let author = pdf.author { lines.append("Author  " + author) }
            lines.append(ByteCountFormatter.string(fromByteCount: info.fileSize, countStyle: .file) + "  ·  PDF document")
            lines.append("Zoom    \(Int((zoom * 100).rounded()))%" + (edited ? "  ·  edited" : ""))
            return lines.joined(separator: "\n")
        }

        let megapixels = size.width * size.height / 1_000_000
        lines.append(String(format: "%d × %d px  (%.1f MP)", Int(size.width), Int(size.height), megapixels))

        var fileLine = ByteCountFormatter.string(fromByteCount: info.fileSize, countStyle: .file)
        if let identifier = info.typeIdentifier, let description = UTType(identifier)?.localizedDescription {
            fileLine += "  ·  " + description
        }
        lines.append(fileLine)

        if info.pageCount > 1 {
            lines.append(info.isAnimated ? "\(info.pageCount) frames" : "Page \(page + 1) of \(info.pageCount)")
        }

        let exif = info.properties[kCGImagePropertyExifDictionary] as? [CFString: Any] ?? [:]
        let tiff = info.properties[kCGImagePropertyTIFFDictionary] as? [CFString: Any] ?? [:]

        if let taken = exif[kCGImagePropertyExifDateTimeOriginal] as? String {
            lines.append("Taken   " + taken)
        }
        let make = (tiff[kCGImagePropertyTIFFMake] as? String)?.trimmingCharacters(in: .whitespaces) ?? ""
        let model = (tiff[kCGImagePropertyTIFFModel] as? String)?.trimmingCharacters(in: .whitespaces) ?? ""
        let camera = model.hasPrefix(make) ? model : [make, model].filter { !$0.isEmpty }.joined(separator: " ")
        if !camera.isEmpty {
            lines.append("Camera  " + camera)
        }
        if let lens = exif[kCGImagePropertyExifLensModel] as? String {
            lines.append("Lens    " + lens)
        }

        var exposure: [String] = []
        if let time = exif[kCGImagePropertyExifExposureTime] as? Double, time > 0 {
            exposure.append(time >= 1 ? String(format: "%.1f s", time) : "1/\(Int((1 / time).rounded())) s")
        }
        if let aperture = exif[kCGImagePropertyExifFNumber] as? Double {
            exposure.append(String(format: "f/%.1f", aperture))
        }
        if let iso = (exif[kCGImagePropertyExifISOSpeedRatings] as? [Int])?.first {
            exposure.append("ISO \(iso)")
        }
        if let focal = exif[kCGImagePropertyExifFocalLength] as? Double {
            exposure.append(String(format: "%.0f mm", focal))
        }
        if !exposure.isEmpty {
            lines.append(exposure.joined(separator: "   "))
        }

        if let gps = info.properties[kCGImagePropertyGPSDictionary] as? [CFString: Any],
           let latitude = gps[kCGImagePropertyGPSLatitude] as? Double,
           let longitude = gps[kCGImagePropertyGPSLongitude] as? Double {
            let lat = (gps[kCGImagePropertyGPSLatitudeRef] as? String) == "S" ? -latitude : latitude
            let lon = (gps[kCGImagePropertyGPSLongitudeRef] as? String) == "W" ? -longitude : longitude
            lines.append(String(format: "GPS     %.5f, %.5f", lat, lon))
        }

        lines.append("Zoom    \(Int((zoom * 100).rounded()))%" + (edited ? "  ·  edited" : ""))
        return lines.joined(separator: "\n")
    }
}
