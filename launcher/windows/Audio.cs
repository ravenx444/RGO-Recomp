using System;
using System.Globalization;
using System.IO;
using System.Text;
namespace JfgLauncher {
    // RGO widescreen: the game reads and follows this profile file live.
    // Format and default are owned by include/jfg/renderer/widescreen.hpp.
    internal static class VideoPreferences {
        internal static string FileName(string profile) { return Path.Combine(profile, "video.ini"); }
    }
    internal sealed class AudioPreferences {
        internal int Volume = 100;
        internal bool Muted;
        internal static string FileName(string profile) { return Path.Combine(profile, "audio.ini"); }
        internal string Encode() {
            if (Volume < 0 || Volume > 100) throw new InvalidDataException("Volume must be between 0 and 100.");
            return "version=1\nvolume=" + Volume.ToString(CultureInfo.InvariantCulture) + "\nmuted=" + (Muted ? "1" : "0") + "\n";
        }
        internal static AudioPreferences Parse(string text) {
            if (text.Length > 128) throw new InvalidDataException("Invalid audio settings.");
            string[] lines = text.Replace("\r\n", "\n").Split('\n');
            int volume;
            if (lines.Length != 4 || lines[0] != "version=1" || lines[3] != "" ||
                !lines[1].StartsWith("volume=", StringComparison.Ordinal) ||
                !Int32.TryParse(lines[1].Substring(7), NumberStyles.None, CultureInfo.InvariantCulture, out volume) ||
                volume < 0 || volume > 100 || volume.ToString(CultureInfo.InvariantCulture) != lines[1].Substring(7) ||
                (lines[2] != "muted=0" && lines[2] != "muted=1"))
                throw new InvalidDataException("Invalid audio settings.");
            return new AudioPreferences { Volume = volume, Muted = lines[2] == "muted=1" };
        }
        internal static AudioPreferences Load(string profile) {
            string path = FileName(profile);
            if (!File.Exists(path)) return new AudioPreferences();
            if (new FileInfo(path).Length > 128) throw new InvalidDataException("Invalid audio settings.");
            return Parse(File.ReadAllText(path, Encoding.ASCII));
        }
        internal void Save(string profile) {
            string text = Encode();
            Directory.CreateDirectory(profile);
            string path = FileName(profile), temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try {
                File.WriteAllText(temporary, text, Encoding.ASCII);
                if (File.Exists(path)) File.Replace(temporary, path, null);
                else File.Move(temporary, path);
            } finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
    }
}
