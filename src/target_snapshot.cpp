#include "target_snapshot.hpp"

#include "base64url.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace alarmmap {

using json = nlohmann::json;

namespace {

constexpr int kCompactVersion = 2;
constexpr size_t kCompactFieldCount = 12;
constexpr int kMaxEnvelopeDepth = 4;
constexpr size_t kMaxRememberedEpochs = 8;
constexpr size_t kGuidSize = 16;
// .NET Guid.ToByteArray stores its first three fields little-endian.
constexpr std::array<size_t, kGuidSize> kGuidByteOrder = {3, 2, 1, 0, 5, 4, 7, 6,
                                                         8, 9, 10, 11, 12, 13, 14, 15};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

// Mirrors JavaScript truthiness for the scalar flags the server sends.
bool isTruthy(const json& value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value.get<double>() != 0;
    if (value.is_string()) return !value.get_ref<const std::string&>().empty();
    return true;
}

// Compact rows may be shorter than their full layout; a missing tail
// element means the same as null.
const json& at(const json& row, size_t index) {
    static const json kNull;
    return index < row.size() ? row[index] : kNull;
}

template <typename T>
std::optional<T> optionalValue(const json& value) {
    if (value.is_null()) return std::nullopt;
    return value.get<T>();
}

template <typename T>
T valueOr(const json& value, T fallback) {
    return value.is_null() ? fallback : value.get<T>();
}

const json& field(const json& object, const char* key) {
    static const json kNull;
    const auto found = object.find(key);
    return found == object.end() ? kNull : *found;
}

const json& unwrapEnvelopes(const json& packet) {
    const json* current = &packet;
    for (int depth = 0; depth < kMaxEnvelopeDepth && current->is_object(); ++depth) {
        if (current->contains("v") || field(*current, "targets").is_array()) return *current;
        const json& inner = field(*current, "data");
        if (!inner.is_object()) break;
        current = &inner;
    }
    return *current;
}

std::string decodeTrackId(const json& value) {
    if (!value.is_string()) fail("invalid track ID reference");
    const std::string& text = value.get_ref<const std::string&>();
    if (text.starts_with("~~")) return text.substr(1);
    if (!text.starts_with("~")) return text;

    const std::string bytes = decodeBase64Url(std::string_view(text).substr(1));
    if (bytes.size() != kGuidSize) fail("track ID is not a 16-byte GUID");
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string hex;
    for (const size_t index : kGuidByteOrder) {
        const auto byte = static_cast<unsigned char>(bytes[index]);
        hex.push_back(kHexDigits[byte >> 4]);
        hex.push_back(kHexDigits[byte & 0x0F]);
    }
    return hex;
}

class CompactDecoder {
public:
    explicit CompactDecoder(const json& fields)
        : time_(fields[0].get<std::int64_t>()),
          cells_(fields[5]),
          styles_(fields[6]) {
        for (const json& id : fields[7]) trackIds_.push_back(decodeTrackId(id));
    }

    std::int64_t timestamp(const json& offset) const {
        return offset.is_null() ? 0 : time_ + offset.get<std::int64_t>();
    }

    Cell cell(const json& reference) const {
        const json& value = cells_[checkedIndex(reference, cells_.size(), "cell")];
        if (!value.is_array() || value.size() < 4) fail("invalid target cell reference");
        return {value[0].get<double>(), value[1].get<double>(), value[2].get<double>(),
                value[3].get<double>(),
                isTruthy(at(value, 4)) ? at(value, 4).get<std::string>() : std::string()};
    }

    Target target(const json& row) const {
        const json& style = styles_[checkedIndex(at(row, 2), styles_.size(), "style")];

        Target target;
        target.cell = cell(at(row, 1));
        target.trackId = trackIds_[checkedIndex(at(row, 0), trackIds_.size(), "track ID")];
        target.type = style[0].get<int>();
        target.formular = style[1].get<std::string>();
        target.detectedType = style[2].get<std::string>();
        target.iconName = style[3].get<std::string>();
        target.iconScale = style[4].get<double>();
        target.headingDegrees = optionalValue<double>(at(row, 3));
        target.currentCount = valueOr(at(row, 4), 0);
        target.hits = valueOr(at(row, 5), 0);
        target.movementDurationMs = valueOr(at(row, 6), 0);
        if (isTruthy(at(row, 7))) target.markerOffsetX = at(row, 7).get<double>();
        if (isTruthy(at(row, 8))) target.markerOffsetY = at(row, 8).get<double>();
        target.isFading = isTruthy(at(row, 9));
        if (!at(row, 10).is_null()) target.fadeStartedAtMs = timestamp(at(row, 10));
        if (!at(row, 11).is_null()) target.fadeEndsAtMs = timestamp(at(row, 11));

        const json& prediction = at(row, 12);
        if (isTruthy(prediction)) {
            target.prediction = Prediction{
                prediction[0].get<double>(),   prediction[1].get<double>(),
                prediction[2].get<double>(),   prediction[3].get<double>(),
                timestamp(at(prediction, 4)), timestamp(at(prediction, 5))};
        }
        return target;
    }

    HeatCell heat(const json& row) const {
        HeatCell heat;
        heat.cell = cell(at(row, 0));
        heat.age = valueOr(at(row, 1), 0.0);
        heat.hits = valueOr(at(row, 2), 0);
        heat.baseStrength = optionalValue<double>(at(row, 3));
        if (isTruthy(at(row, 4))) heat.minimumStrength = at(row, 4).get<double>();
        heat.retentionAge = optionalValue<double>(at(row, 5));
        if (!at(row, 6).is_null()) {
            heat.samples.emplace();
            for (const json& sample : at(row, 6)) heat.samples->push_back(this->sample(sample));
        }
        if (!at(row, 7).is_null()) {
            heat.extrapolationHoldAnchors.emplace();
            for (const json& reference : at(row, 7)) {
                Cell anchor = cell(reference);
                anchor.gridLevel.clear();
                heat.extrapolationHoldAnchors->push_back(anchor);
            }
        }
        heat.predictedExtrapolation = isTruthy(at(row, 8));
        return heat;
    }

private:
    static size_t checkedIndex(const json& reference, size_t tableSize, const char* what) {
        if (!reference.is_number_integer() || reference.get<std::int64_t>() < 0 ||
            reference.get<size_t>() >= tableSize) {
            fail(std::string("invalid target ") + what + " reference");
        }
        return reference.get<size_t>();
    }

    HeatSample sample(const json& row) const {
        return {valueOr(at(row, 0), 0.0),           optionalValue<double>(at(row, 1)),
                valueOr(at(row, 2), 0.0),           optionalValue<double>(at(row, 3)),
                optionalValue<double>(at(row, 4)), optionalValue<double>(at(row, 5)),
                optionalValue<double>(at(row, 6)), optionalValue<double>(at(row, 7)),
                optionalValue<double>(at(row, 8))};
    }

    std::int64_t time_;
    const json& cells_;
    const json& styles_;
    std::vector<std::string> trackIds_;
};

TargetSnapshot decodeCompact(const json& packet) {
    const json& fields = field(packet, "d");
    if (field(packet, "v") != kCompactVersion || !fields.is_array() ||
        fields.size() != kCompactFieldCount) {
        fail("unsupported target snapshot format");
    }
    const CompactDecoder decoder(fields);

    TargetSnapshot snapshot;
    for (const json& row : fields[8]) snapshot.targets.push_back(decoder.target(row));
    for (const json& row : fields[9]) snapshot.trail.push_back(decoder.heat(row));
    for (const json& row : fields[10]) snapshot.extrapolationTrail.push_back(decoder.heat(row));
    snapshot.serverTimeMs = fields[0].get<std::int64_t>();
    snapshot.updatedAtMs = decoder.timestamp(fields[1]);
    snapshot.extrapolationUpdatedAtMs = decoder.timestamp(fields[2]);
    snapshot.fogSeconds = fields[3].get<double>();
    snapshot.snapshotTtlSeconds = fields[4].get<double>();

    const json& movement = fields[11];
    if (!movement.is_null()) {
        Movement decoded;
        for (const json& row : movement[0]) decoded.from.push_back(decoder.target(row));
        decoded.fromUpdatedAtMs = decoder.timestamp(movement[1]);
        decoded.startedAtMs = decoder.timestamp(movement[2]);
        snapshot.movement = std::move(decoded);
    }
    return snapshot;
}

Cell expandedCell(const json& object) {
    return {field(object, "x").get<double>(), field(object, "y").get<double>(),
            field(object, "cellWidth").get<double>(), field(object, "cellHeight").get<double>(),
            valueOr<std::string>(field(object, "gridLevel"), "")};
}

Target expandedTarget(const json& object) {
    Target target;
    target.cell = expandedCell(object);
    target.trackId = field(object, "trackId").get<std::string>();
    target.type = valueOr(field(object, "type"), 0);
    target.formular = valueOr<std::string>(field(object, "formular"), "");
    target.detectedType = valueOr<std::string>(field(object, "detectedType"), "");
    target.iconName = valueOr<std::string>(field(object, "iconName"), "");
    target.iconScale = valueOr(field(object, "iconScale"), 0.0);
    target.headingDegrees = optionalValue<double>(field(object, "heading"));
    target.currentCount = valueOr(field(object, "currentCount"), 0);
    target.hits = valueOr(field(object, "h"), 0);
    target.movementDurationMs = valueOr(field(object, "movementDurationMs"), 0);
    target.markerOffsetX = valueOr(field(object, "markerOffsetX"), 0.0);
    target.markerOffsetY = valueOr(field(object, "markerOffsetY"), 0.0);
    target.isFading = isTruthy(field(object, "isFading"));
    target.fadeStartedAtMs = optionalValue<std::int64_t>(field(object, "fadeStartedAt"));
    target.fadeEndsAtMs = optionalValue<std::int64_t>(field(object, "fadeEndsAt"));
    const json& prediction = field(object, "prediction");
    if (prediction.is_object()) {
        target.prediction = Prediction{
            field(prediction, "fromX").get<double>(), field(prediction, "fromY").get<double>(),
            field(prediction, "toX").get<double>(), field(prediction, "toY").get<double>(),
            field(prediction, "startedAt").get<std::int64_t>(),
            field(prediction, "endsAt").get<std::int64_t>()};
    }
    return target;
}

HeatSample expandedSample(const json& object) {
    return {valueOr(field(object, "age"), 0.0),
            optionalValue<double>(field(object, "baseStrength")),
            valueOr(field(object, "minimumStrength"), 0.0),
            optionalValue<double>(field(object, "retentionAge")),
            optionalValue<double>(field(object, "fadeDelay")),
            optionalValue<double>(field(object, "fadeDuration")),
            optionalValue<double>(field(object, "appearDelay")),
            optionalValue<double>(field(object, "appearDuration")),
            optionalValue<double>(field(object, "visibleUntil"))};
}

HeatCell expandedHeat(const json& object) {
    HeatCell heat;
    heat.cell = expandedCell(object);
    heat.age = valueOr(field(object, "age"), 0.0);
    heat.hits = valueOr(field(object, "h"), 0);
    heat.baseStrength = optionalValue<double>(field(object, "baseStrength"));
    heat.minimumStrength = valueOr(field(object, "minimumStrength"), 0.0);
    heat.retentionAge = optionalValue<double>(field(object, "retentionAge"));
    if (field(object, "samples").is_array()) {
        heat.samples.emplace();
        for (const json& sample : object["samples"]) heat.samples->push_back(expandedSample(sample));
    }
    if (field(object, "extrapolationHoldAnchors").is_array()) {
        heat.extrapolationHoldAnchors.emplace();
        for (const json& anchor : object["extrapolationHoldAnchors"]) {
            heat.extrapolationHoldAnchors->push_back(expandedCell(anchor));
        }
    }
    heat.predictedExtrapolation = isTruthy(field(object, "predictedExtrapolation"));
    return heat;
}

TargetSnapshot decodeExpanded(const json& object) {
    TargetSnapshot snapshot;
    for (const json& target : object["targets"]) snapshot.targets.push_back(expandedTarget(target));
    for (const json& heat : valueOr(field(object, "trail"), json::array())) {
        snapshot.trail.push_back(expandedHeat(heat));
    }
    for (const json& heat : valueOr(field(object, "extrapolationTrail"), json::array())) {
        snapshot.extrapolationTrail.push_back(expandedHeat(heat));
    }
    snapshot.serverTimeMs = valueOr<std::int64_t>(field(object, "serverTime"), 0);
    snapshot.updatedAtMs = valueOr<std::int64_t>(field(object, "updatedAt"), 0);
    snapshot.extrapolationUpdatedAtMs =
        valueOr<std::int64_t>(field(object, "extrapolationUpdatedAt"), 0);
    snapshot.fogSeconds = valueOr(field(object, "fog"), 0.0);
    snapshot.snapshotTtlSeconds = valueOr(field(object, "snapshotTtl"), 0.0);

    const json& movement = field(object, "movement");
    if (movement.is_object()) {
        Movement decoded;
        for (const json& target : movement["from"]) decoded.from.push_back(expandedTarget(target));
        decoded.fromUpdatedAtMs = valueOr<std::int64_t>(field(movement, "fromUpdatedAt"), 0);
        decoded.startedAtMs = valueOr<std::int64_t>(field(movement, "startedAt"), 0);
        snapshot.movement = std::move(decoded);
    }
    return snapshot;
}

bool isDecimal(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(),
                                        [](char symbol) { return symbol >= '0' && symbol <= '9'; });
}

// Revisions are arbitrarily long decimal strings, so compare them as text.
bool isNewerRevision(const std::string& candidate, const std::string& current) {
    if (candidate.size() != current.size()) return candidate.size() > current.size();
    return candidate > current;
}

}  // namespace

TargetSnapshot decodeTargetSnapshot(const json& packet) {
    const json& unwrapped = unwrapEnvelopes(packet);
    if (!unwrapped.is_object()) fail("invalid target snapshot");
    if (unwrapped.contains("v")) return decodeCompact(unwrapped);
    if (!field(unwrapped, "targets").is_array()) fail("invalid target snapshot");
    return decodeExpanded(unwrapped);
}

std::optional<TargetSnapshot> TargetSnapshotReader::read(const json& packet) {
    const json& unwrapped = unwrapEnvelopes(packet);
    TargetSnapshot snapshot = decodeTargetSnapshot(unwrapped);
    if (field(unwrapped, "v") != kCompactVersion) return snapshot;

    const json& revisionField = field(unwrapped, "q");
    const std::string revision = revisionField.is_string()
                                     ? revisionField.get<std::string>()
                                     : revisionField.is_number_unsigned()
                                           ? std::to_string(revisionField.get<std::uint64_t>())
                                           : std::string();
    const json& epochField = field(unwrapped, "e");
    if (!isDecimal(revision) || !epochField.is_string()) fail("invalid snapshot revision");
    const std::string& epoch = epochField.get_ref<const std::string&>();

    if (epoch_ == epoch) {
        if (!isNewerRevision(revision, revision_)) return std::nullopt;
    } else {
        const bool seenBefore =
            std::find(seenEpochs_.begin(), seenEpochs_.end(), epoch) != seenEpochs_.end();
        if (seenBefore || snapshot.serverTimeMs < serverTimeMs_) return std::nullopt;
        seenEpochs_.push_back(epoch);
        if (seenEpochs_.size() > kMaxRememberedEpochs) seenEpochs_.pop_front();
    }
    epoch_ = epoch;
    revision_ = revision;
    serverTimeMs_ = snapshot.serverTimeMs;
    return snapshot;
}

}  // namespace alarmmap
