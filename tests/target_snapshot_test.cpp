// Usage: target_snapshot_test <tests/data directory>
//
// target_packets.jsonl holds recorded compact packets together with the
// output of the site's own JavaScript decoder for the same packet.

#include "packed_packet.hpp"
#include "target_snapshot.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <source_location>
#include <string>
#include <vector>

using json = nlohmann::json;
using alarmmap::decodeTargetSnapshot;
using alarmmap::TargetSnapshot;
using alarmmap::TargetSnapshotReader;

namespace {

int failures = 0;

void check(bool condition, const std::string& what,
           std::source_location location = std::source_location::current()) {
    if (condition) return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": FAILED: " << what << '\n';
}

void checkThrows(const std::function<void()>& action, const std::string& what,
                 std::source_location location = std::source_location::current()) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    check(false, what + " (no exception)", location);
}

json readJson(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return json::parse(input);
}

std::vector<json> readJsonLines(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    std::vector<json> lines;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty()) lines.push_back(json::parse(line));
    }
    return lines;
}

const json kCells = json::array({json::array({0.5, 0.25, 0.01, 0.02, "grid"})});
const json kStyles = json::array({json::array({8, "formular", "Shahed", "ic_shahed", 0.85})});

json compactPacket(const json& ids, const json& targets, const json& trail = json::array(),
                   const std::string& epoch = "e1", const std::string& revision = "1",
                   std::int64_t serverTimeMs = 1000) {
    const json fields = json::array({serverTimeMs, 0, nullptr, 30, 30, kCells, kStyles, ids,
                                     targets, trail, json::array(), nullptr});
    return {{"v", 2}, {"e", epoch}, {"q", revision}, {"d", fields}};
}

json packetAt(const std::string& epoch, const std::string& revision, std::int64_t serverTimeMs) {
    return compactPacket(json::array({"plain"}), json::array(), json::array(), epoch, revision,
                         serverTimeMs);
}

void checkTargetMatchesJson(const alarmmap::Target& target, const json& expected) {
    check(target.trackId == expected["trackId"].get<std::string>(), "trackId");
    check(target.cell.x == expected["x"].get<double>(), "target x");
    check(target.cell.y == expected["y"].get<double>(), "target y");
    check(target.iconName == expected["iconName"].get<std::string>(), "iconName");
    const json& heading = expected["heading"];
    check(heading.is_null() ? !target.headingDegrees
                            : target.headingDegrees == heading.get<double>(),
          "heading");
    check(target.movementDurationMs == expected["movementDurationMs"].get<int>(), "duration");
    check(target.prediction.has_value() == expected.contains("prediction"), "prediction presence");
    if (target.prediction && expected.contains("prediction")) {
        const json& prediction = expected["prediction"];
        check(target.prediction->toX == prediction["toX"].get<double>(), "prediction toX");
        check(target.prediction->startedAtMs == prediction["startedAt"].get<std::int64_t>(),
              "prediction startedAt");
        check(target.prediction->endsAtMs == prediction["endsAt"].get<std::int64_t>(),
              "prediction endsAt");
    }
    check(target.fadeEndsAtMs.has_value() == expected.contains("fadeEndsAt"), "fadeEndsAt");
}

void testMatchesSiteDecoder(const std::filesystem::path& dataDirectory) {
    const auto lines = readJsonLines(dataDirectory / "target_packets.jsonl");
    check(!lines.empty(), "fixture has packets");

    size_t targets = 0, predictions = 0, sampledCells = 0, predictedCells = 0;
    for (const json& line : lines) {
        const json& expected = line["expected"];
        const TargetSnapshot decoded = decodeTargetSnapshot(line["packet"]);
        check(decoded == decodeTargetSnapshot(expected), "compact equals site-decoded snapshot");
        check(decoded.skippedEntries == 0, "a recorded packet is read in full");

        check(decoded.serverTimeMs == expected["serverTime"].get<std::int64_t>(), "serverTime");
        check(decoded.updatedAtMs == expected["updatedAt"].get<std::int64_t>(), "updatedAt");
        check(decoded.targets.size() == expected["targets"].size(), "target count");
        check(decoded.trail.size() == expected["trail"].size(), "trail count");
        check(decoded.extrapolationTrail.size() == expected["extrapolationTrail"].size(),
              "extrapolation count");
        for (size_t i = 0; i < decoded.targets.size() && i < expected["targets"].size(); ++i) {
            checkTargetMatchesJson(decoded.targets[i], expected["targets"][i]);
        }
        for (size_t i = 0; i < decoded.trail.size() && i < expected["trail"].size(); ++i) {
            const json& heat = expected["trail"][i];
            check(decoded.trail[i].age == heat["age"].get<double>(), "trail age");
            check(decoded.trail[i].cell.y == heat["y"].get<double>(), "trail y");
            check(decoded.trail[i].samples.has_value() == heat.contains("samples"),
                  "trail samples presence");
        }

        targets += decoded.targets.size();
        for (const auto& target : decoded.targets) predictions += target.prediction.has_value();
        for (const auto& heat : decoded.trail) sampledCells += heat.samples.has_value();
        for (const auto& heat : decoded.extrapolationTrail) {
            sampledCells += heat.samples.has_value();
            predictedCells += heat.predictedExtrapolation;
        }
    }
    check(targets > 0 && predictions > 0 && sampledCells > 0 && predictedCells > 0,
          "fixture exercises targets, predictions, samples and predicted cells");
}

void testRestSnapshot(const std::filesystem::path& dataDirectory) {
    const json rest = readJson(dataDirectory / "rest_snapshot.json");
    const TargetSnapshot snapshot = decodeTargetSnapshot(rest);
    check(snapshot.skippedEntries == 0, "the recorded REST snapshot is read in full");
    check(snapshot.targets.size() == rest["targets"].size(), "REST target count");
    check(snapshot.trail.size() == rest["trail"].size(), "REST trail count");
    check(snapshot.fogSeconds == rest["fog"].get<double>(), "REST fog");
    check(snapshot.movement.has_value() == rest["movement"].is_object(), "REST movement");
    for (size_t i = 0; i < snapshot.targets.size(); ++i) {
        checkTargetMatchesJson(snapshot.targets[i], rest["targets"][i]);
    }
}

void testTrackIdFormats() {
    const json ids = json::array({"~AAECAwQFBgcICQoLDA0ODw", "~~literal", "plain"});
    const json targets = json::array({json::array({0, 0, 0, 90}), json::array({1, 0, 0, nullptr}),
                                      json::array({2, 0, 0})});
    const TargetSnapshot snapshot = decodeTargetSnapshot(compactPacket(ids, targets));
    check(snapshot.targets.size() == 3, "three targets");
    if (snapshot.targets.size() != 3) return;
    check(snapshot.targets[0].trackId == "0302010005040706" "08090a0b0c0d0e0f", "GUID byte order");
    check(snapshot.targets[1].trackId == "~literal", "escaped tilde");
    check(snapshot.targets[2].trackId == "plain", "literal ID");
    check(snapshot.targets[0].headingDegrees == 90.0, "heading value");
    check(!snapshot.targets[1].headingDegrees && !snapshot.targets[2].headingDegrees,
          "null and missing heading");
    check(snapshot.targets[2].currentCount == 0 && snapshot.targets[2].markerOffsetX == 0,
          "missing tail fields default to zero");
    check(snapshot.targets[0].formular == "formular" && snapshot.targets[0].iconScale == 0.85,
          "style fields");

    const TargetSnapshot shortGuid = decodeTargetSnapshot(
        compactPacket(json::array({"~AAEC", "plain"}), json::array({json::array({0, 0, 0}), json::array({1, 0, 0})})));
    check(shortGuid.targets.size() == 1 && shortGuid.targets[0].trackId == "plain" && shortGuid.skippedEntries == 2,
          "a short GUID costs its target, the others stay");
}

void testAnchors() {
    const json trail = json::array({json::array({0, 5, 2, nullptr, nullptr, nullptr, nullptr,
                                                 json::array({0})})});
    const TargetSnapshot snapshot =
        decodeTargetSnapshot(compactPacket(json::array(), json::array(), trail));
    check(snapshot.trail.size() == 1, "one heat cell");
    if (snapshot.trail.empty()) return;
    const auto& heat = snapshot.trail[0];
    check(heat.cell.gridLevel == "grid", "heat cell keeps grid level");
    check(heat.extrapolationHoldAnchors && heat.extrapolationHoldAnchors->size() == 1,
          "anchor decoded");
    if (heat.extrapolationHoldAnchors && !heat.extrapolationHoldAnchors->empty()) {
        const auto& anchor = heat.extrapolationHoldAnchors->front();
        check(anchor.x == 0.5 && anchor.height == 0.02 && anchor.gridLevel.empty(),
              "anchor geometry without grid level");
    }
    check(!heat.samples && !heat.baseStrength && heat.minimumStrength == 0, "absent optionals");
}

void testCentrifugoEnvelope() {
    const json packet = compactPacket(json::array({"plain"}), json::array({json::array({0, 0, 0})}));
    const json envelope = {{"data", {{"data", packet}, {"offset", 5}}}};
    check(decodeTargetSnapshot(envelope) == decodeTargetSnapshot(packet), "envelope unwrapped");
}

// packed_packets.jsonl pairs recorded packed text with the output of the
// site's own JavaScript decoder for it.
void testPackedPackets(const std::filesystem::path& dataDirectory) {
    const auto lines = readJsonLines(dataDirectory / "packed_packets.jsonl");
    check(!lines.empty(), "fixture has packed packets");

    size_t targets = 0, predictions = 0;
    for (const json& line : lines) {
        const std::string& packed = line["packed"].get_ref<const std::string&>();
        const TargetSnapshot expected = decodeTargetSnapshot(line["expected"]);
        const TargetSnapshot decoded = decodeTargetSnapshot(line["packed"]);
        check(decoded == expected, "packed equals site-decoded snapshot");
        check(decoded.skippedEntries == 0, "a recorded packed packet is read in full");
        check(decodeTargetSnapshot(json{{"data", {{"data", packed}}}}) == expected, "packed text in envelopes");

        targets += decoded.targets.size();
        for (const auto& target : decoded.targets) predictions += target.prediction.has_value();
    }
    check(targets > 0 && predictions > 0, "fixture exercises targets and predictions");

    const json first = alarmmap::unpackPacket(lines[0]["packed"].get_ref<const std::string&>());
    check(first["v"] == 2 && first["e"].is_string() && first["q"].is_string(), "unpacked header");
    TargetSnapshotReader reader;
    check(reader.read(lines[0]["packed"]).has_value() && !reader.read(lines[0]["packed"]),
          "a repeated packed packet is dropped");
}

void testRejectsMalformedPacked(const std::filesystem::path& dataDirectory) {
    const std::string packed = readJsonLines(dataDirectory / "packed_packets.jsonl")[0]["packed"];

    std::string corrupted = packed;
    corrupted[corrupted.size() / 2] = corrupted[corrupted.size() / 2] == 'A' ? 'B' : 'A';
    checkThrows([&] { alarmmap::unpackPacket(corrupted); }, "corrupted body fails the checksum");
    checkThrows([&] { alarmmap::unpackPacket(packed.substr(0, packed.size() / 2)); }, "truncated text");
    checkThrows([&] { alarmmap::unpackPacket("~3:"); }, "empty body");
    checkThrows([&] { alarmmap::unpackPacket("~3:AAAAAAAAAAAAAAAA"); }, "arbitrary body");
    checkThrows([&] { alarmmap::unpackPacket("~4:" + packed.substr(3)); }, "unknown prefix");
    checkThrows([&] { alarmmap::unpackPacket(packed + "*"); }, "foreign character");
    check(!alarmmap::isPackedPacket(json{{"data", packed}}) && alarmmap::isPackedPacket(json(packed)),
          "only a bare string counts as packed");
}

void testRejectsMalformed() {
    json packet = compactPacket(json::array({"plain"}), json::array({json::array({0, 0, 0})}));

    json shortFields = packet;
    shortFields["d"].erase(11);
    checkThrows([&] { decodeTargetSnapshot(shortFields); }, "11 fields");

    json badVersion = packet;
    badVersion["v"] = 3;
    checkThrows([&] { decodeTargetSnapshot(badVersion); }, "unknown version");

    checkThrows([] { decodeTargetSnapshot(json{{"unrelated", 1}}); }, "unrecognized object");

    json noTime = packet;
    noTime["d"][0] = nullptr;
    checkThrows([&] { decodeTargetSnapshot(noTime); }, "a snapshot without its server time");
}

// One unreadable entry costs only itself: the rest of the snapshot is shown.
void testDegradesGracefully() {
    const json ids = json::array({"first", "second"});
    const json rows = json::array({json::array({0, 0, 0}), json::array({1, 0, 0})});
    const auto keepsOne = [&](const char* what, const std::function<void(json&)>& spoil) {
        json packet = compactPacket(ids, rows);
        spoil(packet["d"]);
        const TargetSnapshot snapshot = decodeTargetSnapshot(packet);
        check(snapshot.targets.size() == 1 && snapshot.targets[0].trackId == "second" &&
                  snapshot.skippedEntries == 1,
              what);
    };
    keepsOne("a target with its cell out of range is left out", [](json& d) { d[8][0][1] = 5; });
    keepsOne("a target with its style out of range is left out", [](json& d) { d[8][0][2] = 3; });
    keepsOne("a target with a negative track ID reference is left out", [](json& d) { d[8][0][0] = -1; });
    keepsOne("a target row that is not a row is left out", [](json& d) { d[8][0] = "odd"; });

    // Both the unreadable ID and the target using it count, so an unused bad
    // ID is noticed too.
    json badId = compactPacket(ids, rows);
    badId["d"][7][0] = 42;
    const TargetSnapshot withoutId = decodeTargetSnapshot(badId);
    check(withoutId.targets.size() == 1 && withoutId.targets[0].trackId == "second" && withoutId.skippedEntries == 2,
          "a target whose track ID cannot be decoded is left out");

    json trail = compactPacket(ids, rows, json::array({json::array({0, 1.5, 2}), json::array({7, 1, 1})}));
    const TargetSnapshot trailed = decodeTargetSnapshot(trail);
    check(trailed.trail.size() == 1 && trailed.targets.size() == 2 && trailed.skippedEntries == 1,
          "an unreadable heat cell is left out, the targets stay");

    json extended = compactPacket(ids, rows);
    extended["d"].push_back("a field added later");
    const TargetSnapshot widened = decodeTargetSnapshot(extended);
    check(widened.targets.size() == 2 && widened.skippedEntries == 0, "fields added at the end are ignored");

    json noFog = compactPacket(ids, rows);
    noFog["d"][3] = nullptr;
    const TargetSnapshot fogless = decodeTargetSnapshot(noFog);
    check(fogless.fogSeconds == 30 && fogless.targets.size() == 2 && fogless.skippedEntries == 1,
          "a missing fog takes the site's default");

    json rest = {{"targets", json::array({{{"trackId", "a"}, {"x", 0.5}, {"y", 0.5}, {"cellWidth", 0.01},
                                           {"cellHeight", 0.02}, {"type", 8}},
                                          "not a target"})},
                 {"serverTime", 1000}};
    const TargetSnapshot expanded = decodeTargetSnapshot(rest);
    check(expanded.targets.size() == 1 && expanded.skippedEntries == 1,
          "an unreadable target in a REST snapshot is left out");
}

void testReaderOrdering() {
    TargetSnapshotReader reader;
    check(reader.read(packetAt("e1", "441", 1000)).has_value(), "first packet accepted");
    check(!reader.read(packetAt("e1", "441", 1000)), "duplicate revision dropped");
    check(!reader.read(packetAt("e1", "440", 1100)), "older revision dropped");
    check(reader.read(packetAt("e1", "1000", 1100)).has_value(), "longer revision is newer");
    check(reader.read(packetAt("e2", "1", 1200)).has_value(), "new epoch accepted");
    check(!reader.read(packetAt("e1", "2000", 1300)), "previously seen epoch dropped");
    check(!reader.read(packetAt("e3", "1", 900)), "new epoch with older server time dropped");
    check(reader.read(packetAt("e3", "1", 1300)).has_value(), "new epoch with newer time");

    json invalidRevision = packetAt("e3", "12a", 1400);
    checkThrows([&] { reader.read(invalidRevision); }, "non-decimal revision");

    const json expanded = {{"targets", json::array()}};
    check(reader.read(expanded).has_value() && reader.read(expanded).has_value(),
          "expanded snapshots bypass ordering");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: target_snapshot_test <data directory>\n";
        return 2;
    }
    const std::filesystem::path dataDirectory = argv[1];
    try {
        testMatchesSiteDecoder(dataDirectory);
        testRestSnapshot(dataDirectory);
        testTrackIdFormats();
        testAnchors();
        testCentrifugoEnvelope();
        testPackedPackets(dataDirectory);
        testRejectsMalformedPacked(dataDirectory);
        testRejectsMalformed();
        testDegradesGracefully();
        testReaderOrdering();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
