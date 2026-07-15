import AVFoundation
import Foundation

enum MediaMuxer {
    static func addOriginalAudio(videoURL: URL, sourceURL: URL, destinationURL: URL) async throws {
        let videoAsset = AVURLAsset(url: videoURL)
        let sourceAsset = AVURLAsset(url: sourceURL)
        guard let videoTrack = try await videoAsset.loadTracks(withMediaType: .video).first else {
            throw VideoExporterError.cannotConfigure
        }
        guard let audioTrack = try await sourceAsset.loadTracks(withMediaType: .audio).first else {
            try? FileManager.default.removeItem(at: destinationURL)
            try FileManager.default.moveItem(at: videoURL, to: destinationURL)
            return
        }
        let composition = AVMutableComposition()
        guard let compositionVideo = composition.addMutableTrack(withMediaType: .video, preferredTrackID: kCMPersistentTrackID_Invalid),
              let compositionAudio = composition.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid) else {
            throw VideoExporterError.cannotConfigure
        }
        let videoDuration = try await videoAsset.load(.duration)
        let sourceDuration = try await sourceAsset.load(.duration)
        try compositionVideo.insertTimeRange(CMTimeRange(start: .zero, duration: videoDuration), of: videoTrack, at: .zero)
        compositionVideo.preferredTransform = try await videoTrack.load(.preferredTransform)
        let audioDuration = CMTimeMinimum(videoDuration, sourceDuration)
        try compositionAudio.insertTimeRange(CMTimeRange(start: .zero, duration: audioDuration), of: audioTrack, at: .zero)

        try? FileManager.default.removeItem(at: destinationURL)
        guard let session = AVAssetExportSession(asset: composition, presetName: AVAssetExportPresetPassthrough) else {
            throw VideoExporterError.cannotConfigure
        }
        session.outputURL = destinationURL
        session.outputFileType = .mp4
        await session.export()
        guard session.status == .completed else { throw session.error ?? VideoExporterError.cannotConfigure }
        try? FileManager.default.removeItem(at: videoURL)
    }
}
