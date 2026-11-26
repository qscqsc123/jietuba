#pragma once
#include <QString>
#include <stdexcept>

// MP4 recording parameters, saved apart from the GIF frame cache settings.
namespace jietuba {
class ToolSettingsManager;

struct RecordingOptions {
    int fps = 30;
    int bitrate = 4000000;
    bool system_audio = true;
    bool hardware = true;
    bool cursor = true;
    // Throws std::invalid_argument when a value is out of range.
    void validate() const {
        if (fps < 1 || fps > 60) throw std::invalid_argument("Recording FPS must be between 1 and 60");
        if (bitrate < 500000 || bitrate > 50000000) throw std::invalid_argument("Video bitrate must be between 500000 and 50000000 bits/s");
    }
    // A damaged or outdated single setting falls back alone, leaving the other valid ones.
    static RecordingOptions from_config(const ToolSettingsManager *config);
    void save_to_config(ToolSettingsManager *config) const;
};

QString get_recording_mode(const ToolSettingsManager *config);
// Throws std::invalid_argument for anything but "gif" or "mp4".
void set_recording_mode(ToolSettingsManager *config, const QString &mode);
}
