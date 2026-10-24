#include "review_view.hpp"
#include "test_support.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace ow;
int main() {
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("ow-review-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        ReviewView v;
        v.startup.terrainGrid = {64, .5f};
        v.startup.terrainCapacity = 64;
        v.startup.lod = {false, 8, 64};
        v.startup.curvedHorizon = true;
        v.startup.materialDirectory = "owned-test-palette";
        v.startup.streaming.enabled = v.startup.streaming.hierarchical = true;
        v.camera.chunk = {-123456, 654321};
        v.terrainOrigin = {-123460, 654320};
        v.camera.local = {12.125, 1478.25, 3.5};
        v.camera.speed = 5;
        v.camera.pitch = -.7f;
        v.camera.yaw = 1.2f;
        v.terrain.seed = 771;
        v.terrain.wavelength = 1772;
        v.terrain.heightScale = 2000;
        v.scene.freezeLod = true;
        v.scene.taa.thinGeometry = false;
        v.scene.vegetation.distance = 193;
        v.scene.vegetation.density = .83f;
        v.scene.vegetation.occlusion = true;
        v.scene.vegetation.habitat = vegetationHabitat(VegetationHabitat::Moor);
        v.scene.shadows.biasTexels = 1.75f;
        v.scene.visibility = VisibilityMode::HiZ;
        v.scene.terrainMaterial.macroRepeatMetres = 128;
        v.scene.atmosphere.mode = AtmosphereMode::Physical;
        v.scene.atmosphere.aerosolDensity = 1.554f;
        v.lod = {v.camera, 1.75f, 935, true};
        v.lod.observer.local.x -= 13;
        v.lod.patches = {{-8, 0, 1}, {0, 0, 1}};
        const auto path = dir / "view.json";
        saveReviewView(path, v);
        {
            std::ifstream input(path);
            auto legacy = nlohmann::json::parse(input);
            legacy["scene"]["taa"].erase("thinGeometry");
            const auto legacyPath = dir / "legacy-taa.json";
            std::ofstream(legacyPath) << legacy.dump();
            require(loadReviewView(legacyPath).scene.taa.thinGeometry, "Old bookmarks enable thin coverage");
        }
        for (auto mode : {AntialiasingMode::Off, AntialiasingMode::Fxaa, AntialiasingMode::Smaa1x,
                          AntialiasingMode::Taa}) {
            v.scene.antialiasing = mode;
            saveReviewView(path, v);
            require(loadReviewView(path).scene.antialiasing == mode, "Every AA mode round trips");
        }
        for (int version : {1, 2, 3})
            for (bool enabled : {false, true}) {
                std::ifstream input(path);
                auto legacy = nlohmann::json::parse(input);
                legacy["version"] = version;
                legacy["scene"].erase("antialiasing");
                legacy["scene"]["taa"]["enabled"] = enabled;
                legacy["startup"].erase("spatialAaBudgetBytes");
                const auto oldPath = dir / "legacy-aa.json";
                std::ofstream(oldPath) << legacy;
                const auto old = loadReviewView(oldPath);
                require(old.scene.antialiasing == (enabled ? AntialiasingMode::Taa : AntialiasingMode::Off),
                        "Legacy AA choice is preserved");
                require(old.startup.spatialAaBudgetBytes == RendererOptions{}.spatialAaBudgetBytes,
                        "Legacy spatial budget defaults");
            }
        const auto loaded = loadReviewView(path);
        require(!loaded.startup.lod.enabled, "Terrain LOD enable state survives AA bookmark migration");
        require(!loaded.scene.taa.thinGeometry, "Thin-geometry comparison setting round trips");
        require(loaded.camera.chunk.x == v.camera.chunk.x && loaded.camera.local == v.camera.local &&
                    loaded.camera.speed == 5 && loaded.terrain.seed == 771 &&
                    loaded.terrainOrigin.x == v.terrainOrigin.x,
                "camera and terrain round trip");
        require(loaded.lod.patches == v.lod.patches && loaded.lod.observer.local == v.lod.observer.local &&
                    loaded.lod.qualityScale == 1.75f && loaded.scene.freezeLod &&
                    loaded.scene.shadows.biasTexels == 1.75f &&
                    loaded.scene.terrainMaterial.macroRepeatMetres == 128 &&
                    loaded.scene.atmosphere.aerosolDensity == 1.554f,
                "scene and frozen observer round trip");
        require(loaded.scene.vegetation.distance == 193 && loaded.scene.vegetation.density == .83f &&
                    loaded.scene.vegetation.occlusion && loaded.scene.vegetation.habitat.weights.y == 1,
                "vegetation settings survive explicit review bookmarks");
        RendererOptions applied;
        applied.shaderDirectory = "retained";
        applied.streaming.diskCache = false;
        applyReviewStartup(applied, loaded);
        require(compatibleReviewStartup(applied, loaded) && applied.shaderDirectory == "retained" &&
                    !applied.streaming.diskCache,
                "replay preserves process-local options");
        require(applied.materialDirectory == v.startup.materialDirectory,
                "material directory reference round trip");
        applied.renderHeight = 180;
        require(compatibleReviewStartup(applied, loaded), "live resolution compatible");
        applied.lod.patchCells = 16;
        require(!compatibleReviewStartup(applied, loaded), "startup mismatch detected");
        auto invalid = v;
        invalid.version = 99;
        rejects([&] { saveReviewView(path, invalid); }, "unsupported version rejected before replacing file");
        require(loadReviewView(path).version == 4, "invalid save leaves existing view intact");
        nlohmann::json legacy;
        {
            std::ifstream input(path);
            input >> legacy;
        }
        legacy["version"] = 1;
        legacy["scene"].erase("vegetation");
        legacy["lod"].erase("patches");
        legacy["startup"]["lod"].erase("triangleBudget");
        std::ofstream(dir / "legacy.json") << legacy;
        const auto old = loadReviewView(dir / "legacy.json");
        require(old.version == 1 && old.lod.patches.empty() && old.startup.lod.triangleBudget == 2097152,
                "v1 bookmarks retain original settings with current resource defaults");
        require(old.scene.vegetation.enabled && old.scene.vegetation.distance == 80,
                "old bookmarks acquire engine vegetation defaults");
        saveReviewView(dir / "legacy.json", old);
        require(loadReviewView(dir / "legacy.json").version == 4, "new saves upgrade v1 bookmarks");
        for (auto keys : {std::vector<TerrainPatchKey>{{0, 0, 32}},
                          {{1, 0, 0}},
                          {{0, 0, 0}, {0, 0, 1}},
                          {{0, 0, 0}, {0, 0, 0}}}) {
            invalid = v;
            invalid.lod.patches = keys;
            rejects([&] { saveReviewView(path, invalid); }, "invalid frozen partition rejected");
            require(loadReviewView(path).lod.patches == v.lod.patches,
                    "failed partition save preserves the bookmark");
        }
        invalid = v;
        invalid.camera.local.x = INFINITY;
        rejects([&] { saveReviewView(path, invalid); }, "nonfinite camera rejected");
        invalid = v;
        invalid.startup.normalSubdivisions = 4;
        rejects([&] { saveReviewView(path, invalid); }, "unsupported normal subdivision rejected");
        invalid = v;
        invalid.scene.antialiasing = AntialiasingMode(99);
        rejects([&] { saveReviewView(path, invalid); }, "invalid AA enum rejected");
        invalid = v;
        invalid.scene.visibility = VisibilityMode(99);
        rejects([&] { saveReviewView(path, invalid); }, "invalid enum rejected");
        std::ofstream(dir / "bad.json") << "{broken";
        bool failed = false;
        try {
            loadReviewView(dir / "bad.json");
        } catch (const std::exception &) {
            failed = true;
        }
        require(failed, "truncated input rejected");
        failed = false;
        try {
            loadReviewView(dir / "missing.json");
        } catch (const std::exception &) {
            failed = true;
        }
        require(failed, "missing file rejected");
        const auto blocked = dir / "blocked.json";
        std::filesystem::create_directory(blocked);
        failed = false;
        try {
            saveReviewView(blocked, v);
        } catch (const std::exception &) {
            failed = true;
        }
        require(failed && std::filesystem::is_directory(blocked),
                "failed publication leaves destination intact");
        saveReviewView(path, v);
        saveReviewView(path, loaded);
        for (const auto &entry : std::filesystem::directory_iterator(dir))
            require(entry.path().extension() == ".json", "atomic saves leave no temporary files");
        std::filesystem::remove_all(dir);
        std::cout << "Review view tests passed\n";
    } catch (const std::exception &e) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
