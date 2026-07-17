import Foundation

struct CreativeSessionState: Equatable, Sendable {
    var primaryMediaID: UUID?
    var effects: [EffectNode]
    var outputNodeID: UUID?
    var selectedNodeID: UUID?
    var spatialPrefilter: PrefilterStrength
    var temporalPrefilter: PrefilterStrength
}
