using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;
using System.Threading.Tasks;

namespace JfgLauncher {
// Presentation-free adapter. The native RmlUi window owns all controls,
// layout, map projection and input. Existing telemetry and routing remain here.
internal sealed class NativeLiveTools {
  [DllImport("user32.dll")]
  private static extern bool IsWindow(IntPtr window);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  private static extern IntPtr GetProp(IntPtr window, string name);
  private readonly string profile, output;
  private readonly bool inventoryOnly;
  private readonly MapModPreferences modPreferences;
  private readonly IntPtr toolWindow;
  private bool Paused {
    get {
      IntPtr host = GetProp(toolWindow, "JfgLiveOwner");
      return host != IntPtr.Zero &&
             GetProp(host, "JfgFrontendPaused") != IntPtr.Zero;
    }
  }
  private string notice = "";
  private long noticeUntil;
  private string directory = "",
                 status = "Waiting for gameplay and room geometry...",
                 artStatus = "Reading images from your ROM...",
                 aiStatus = "AI stopped";
  private MapSnapshot map;
  private MapGeometry cached;
  private MapLayers layers;
  private FloorFollower follower = new FloorFollower();
  private readonly List<HeightPoint> trail = new List<HeightPoint>();
  private long lastUpdate = -1, nonce = DateTime.UtcNow.Ticks;
  private AutonomousExplorer explorer =
      new AutonomousExplorer(null, new NavigationExplorer());
  private NavigationRoute route;
  private int mode, jump = 8, sequence;
  private float height, slice = 64;
  private bool other = true, collision = true, origins, active;
  private volatile bool artReady;
  private uint selected;
  private InventoryTracker inventory;
  private sealed class Triangle {
    internal HeightPoint A, B, C;
    internal uint Color;
  }
  private sealed class Segment {
    internal HeightPoint A, B;
    internal float Width;
    internal uint Color;
  }
  private sealed class Marker {
    internal uint Id, Color;
    internal float[] Position;
    internal int Shape;
    internal string Label, Details, Kind, Action;
  }
  internal static Process OpenWindow(string kind, string root, IntPtr owner) {
    if (kind != "map" && kind != "inventory")
      throw new ArgumentException("Unknown live tool.");
    string launcher = Environment.GetEnvironmentVariable("JFG_LAUNCHER_EXE");
    if (String.IsNullOrEmpty(launcher) || !File.Exists(launcher))
      launcher = Path.Combine(AppDomain.CurrentDomain.BaseDirectory,
                              "RGO-Launcher.exe");
    if (!File.Exists(launcher))
      throw new IOException("Open live tools from the current RGO launcher.");
    string helper = System.Reflection.Assembly.GetExecutingAssembly().Location;
    return Process.Start(new ProcessStartInfo(
        launcher,
        "--live-tool " + kind + " --profile " + LocalSetup.Quote(root) +
            " --helper " + LocalSetup.Quote(helper) + " --owner " +
            owner.ToInt64().ToString(
                CultureInfo.InvariantCulture)) { UseShellExecute = false });
  }
  internal NativeLiveTools(string root, IntPtr owner, bool inventoryWindow) {
    profile = root;
    toolWindow = owner;
    output = Path.Combine(
        root, "live-tool-" +
                  owner.ToInt64().ToString(CultureInfo.InvariantCulture) + "-" +
                  GetProp(owner, "JfgLiveToken")
                      .ToInt64()
                      .ToString(CultureInfo.InvariantCulture));
    inventoryOnly = inventoryWindow;
    Directory.CreateDirectory(output);
    modPreferences=new MapModPreferences(profile);
  }
  internal static async Task<int> Run(string root, IntPtr owner,
                                      bool inventoryWindow) {
    var service = new NativeLiveTools(root, owner, inventoryWindow);
    Task art =
        inventoryWindow ? Task.Run((Action)service.Art) : Task.FromResult(0);
    try {
      while (IsWindow(owner)) {
        try {
          service.Refresh();
          service.Commands();
          service.Publish();
        } catch (IOException) {
        } catch (UnauthorizedAccessException) {
        }
        await Task.Delay(200);
      }
    } finally {
      service.Stop();
    }
    await art;
    return 0;
  }
  private static uint Tint(Color c, int alpha = 255) {
    return (uint)(c.R << 24 | c.G << 16 | c.B << 8 | alpha);
  }
  private static void Text(BinaryWriter w, string text) {
    byte[] bytes = Encoding.UTF8.GetBytes(text ?? "");
    w.Write(bytes.Length);
    w.Write(bytes);
  }
  private static void Point(BinaryWriter w, HeightPoint p) {
    w.Write(p.X);
    w.Write(p.Z);
  }
  private static uint Bits(bool[] values) {
    uint result = 0;
    if (values != null)
      for (int i = 0; i < Math.Min(32, values.Length); i++)
        if (values[i])
          result |= 1u << i;
    return result;
  }
  private void Art() {
    try {
      string rom = File.ReadAllText(Path.Combine(profile, "frontend-rom.txt"),
                                    Encoding.UTF8)
                       .TrimEnd('\r', '\n');
      var images = InventoryImages.Load(rom);
      try {
        foreach (var item in images) {
          string file = Path.Combine(output, "asset-" + item.Key + ".tga");
          using (var stream = File.Create(file + ".tmp")) using (
              var w = new BinaryWriter(stream)) {
            var image = item.Value;
            byte[] header = new byte[18];
            header[2] = 2;
            header[12] = (byte)image.Width;
            header[13] = (byte)(image.Width >> 8);
            header[14] = (byte)image.Height;
            header[15] = (byte)(image.Height >> 8);
            header[16] = 32;
            header[17] = 40;
            w.Write(header);
            for (int y = 0; y < image.Height; y++)
              for (int x = 0; x < image.Width; x++) {
                Color c = image.GetPixel(x, y);
                w.Write(c.B);
                w.Write(c.G);
                w.Write(c.R);
                w.Write(c.A);
              }
          }
          if (File.Exists(file))
            File.Replace(file + ".tmp", file, null);
          else
            File.Move(file + ".tmp", file);
        }
        artStatus = "Images extracted locally from your game ROM.";
        artReady = true;
      } finally {
        foreach (var image in images.Values)
          image.Dispose();
      }
    } catch (Exception e) {
      if (!Expected(e) && !(e is ArgumentException))
        throw;
      artStatus = "Select a supported ROM in the launcher to see item images.";
    }
  }
  private static bool Expected(Exception e) {
    return e is IOException || e is UnauthorizedAccessException ||
           e is SerializationException || e is InvalidDataException;
  }
  private void Stop() {
    explorer.Stop("AI stopped");
    aiStatus = "AI stopped";
    if (route != null && !explorer.Busy && !String.IsNullOrEmpty(directory))
      try {
        route.Send(directory, ++nonce, false, true);
      } catch (IOException) {
      } catch (UnauthorizedAccessException) {
      }
    route = null;
  }
  private void Commands() {
    string[] files = Directory.GetFiles(output, "command-*.txt");
    Array.Sort(files, StringComparer.Ordinal);
    foreach (string file in files) {
      try {
        if (new FileInfo(file).Length > 1024)
          throw new InvalidDataException("Command too large.");
        string[] fields = File.ReadAllText(file).Trim().Split(' ');
        string command = fields[0];
        float number = 0;
        if (fields.Length > 1 &&
            !Single.TryParse(fields[1], NumberStyles.Float,
                             CultureInfo.InvariantCulture, out number))
          throw new InvalidDataException("Invalid setting.");
        if (Single.IsNaN(number) || Single.IsInfinity(number))
          throw new InvalidDataException("Invalid setting.");
        if (command == "mode")
          mode = Math.Max(0, Math.Min(2, (int)number));
        else if (command == "height")
          height = Math.Max(-1000000, Math.Min(1000000, number));
        else if (command == "slice")
          slice = Math.Max(4, Math.Min(4096, number));
        else if (command == "other")
          other = number != 0;
        else if (command == "collision")
          collision = number != 0;
        else if (command == "origins")
          origins = number != 0;
        else if (command == "warp-mod" || command == "health-mod" || command == "kill-mod" || command == "mods-off") {
          if (modPreferences.Failed && command != "mods-off") continue;
          var current=modPreferences.Values;
          var mods = new MapMods {Warp=current.Warp,Health=current.Health,Kill=current.Kill};
          if (command == "warp-mod") mods.Warp = number != 0;
          if (command == "health-mod") mods.Health = number != 0;
          if (command == "kill-mod") mods.Kill = number != 0;
          if (command == "mods-off") mods.Warp = mods.Health = mods.Kill = false;
          modPreferences.Choose(mods);
          modPreferences.Sync(directory,map==null?null:map.Live,active);
        }
        else if (command == "warp") {
          uint address;
          if (fields.Length != 2 || !UInt32.TryParse(fields[1], out address) || !active)
            throw new InvalidDataException("Invalid exit warp.");
          var mods = MapMods.Load(directory, map == null ? null : map.Live);
          Stop();
          mods.WarpTo(directory, map, address, ++nonce, Paused);
          notice = "Exit warp requested. Normal exit requirements still apply.";
          noticeUntil = NavigationExplorer.Clock + 5000;
        }
        else if (command == "jump")
          jump = number == 0 ? 8 : 32768;
        else if (command == "select") {
          uint address;
          if (fields.Length == 2 && UInt32.TryParse(fields[1], out address))
            selected = address;
        } else if (command == "stop")
          Stop();
        else if (command == "exports") {
          if (!active || !Directory.Exists(directory))
            throw new InvalidDataException(
                "No export folder yet. Start a game and load a room first.");
          Process.Start(new ProcessStartInfo { FileName = directory,
                                               UseShellExecute = true,
                                               Verb = "open" });
          notice = "Opened the current game's export folder.";
          noticeUntil = NavigationExplorer.Clock + 5000;
        } else if (command == "explore" || command == "plan" ||
                   command == "start" || command == "retry") {
          if (map == null || (!map.IsLive && !Paused) ||
              !map.Live.navigation_enabled || !map.Live.clearing_active)
            throw new InvalidDataException(
                "Autopilot requires an active navigation-enabled session.");
          explorer.JumpButton = jump;
          if (command == "explore") {
            Stop();
            explorer.Start(map, NavigationExplorer.Clock);
          }
          if (command == "retry") {
            Stop();
            explorer.RetryRoom(map, NavigationExplorer.Clock);
          }
          if (command == "plan") {
            Stop();
            var marker = Array.Find(map.Live.exits, delegate(MapMarker m) {
              return m.address == selected;
            });
            if (marker == null)
              throw new InvalidDataException(
                  "Select an exit in the inspector first.");
            route = NavigationPlanner.ToExit(map, marker);
            aiStatus = "Route planned: " + route.Points.Count + " waypoints.";
          }
          if (command == "start") {
            if (route == null)
              throw new InvalidDataException("Plan an exit route first.");
            explorer.StartRoute(map, route, NavigationExplorer.Clock);
          }
        }
      } catch (Exception e) {
        if (!Expected(e) && !(e is System.ComponentModel.Win32Exception))
          throw;
        notice = e.Message;
        noticeUntil = NavigationExplorer.Clock + 5000;
        aiStatus = e.Message;
      } finally {
        try {
          File.Delete(file);
        } catch (IOException) {
        } catch (UnauthorizedAccessException) {
        }
      }
    }
  }
  private void Refresh() {
    active = InventorySession.IsActive(profile);
    string next = FrontendBridge.MapDirectory(profile);
    if(!inventoryOnly)modPreferences.Sync(next,null,active);
    if (next != directory) {
      Stop();
      directory = next;
      map = null;
      cached = null;
      layers = null;
      inventory = null;
      follower = new FloorFollower();
      trail.Clear();
      lastUpdate = -1;
      try {
        explorer = AutonomousExplorer.Load(directory);
      } catch (IOException) {
        explorer = new AutonomousExplorer(directory, new NavigationExplorer());
      }
    }
    if (!active) {
      map = null;
      inventory = null;
      cached = null;
      layers = null;
      trail.Clear();
      status = "No game running. Start a game from the launcher.";
      return;
    }
    try {
      if (inventoryOnly) {
        using (var file = new FileStream(
                   Path.Combine(directory, "live.json"), FileMode.Open,
                   FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
          if (file.Length > 16 * 1024 * 1024)
            throw new InvalidDataException("Inventory export is too large.");
          var live = (MapLive) new DataContractJsonSerializer(typeof(MapLive))
                         .ReadObject(file);
          long age =
              live == null ? -1 : NavigationExplorer.Clock - live.timestamp_ms;
          if (live == null || live.schema != 1 || age < 0 ||
              (age > 5000 && !Paused))
            throw new InvalidDataException(
                "Waiting for live inventory data...");
          inventory = live.inventory_tracker;
          if (inventory != null)
            inventory.Validate();
          status = inventory != null && inventory.known
                       ? "Live inventory"
                       : "Waiting for live inventory data...";
        }
        return;
      }
      var value = MapSnapshot.Load(directory, cached);
      if (!value.IsLive && !Paused)
        throw new InvalidDataException("Waiting for fresh game data...");
      bool changed = map == null || value.Live.level != map.Live.level ||
                     value.Live.generation != map.Live.generation;
      if (changed) {
        follower = new FloorFollower();
        trail.Clear();
        lastUpdate = -1;
        selected = 0;
      }
      if (!Object.ReferenceEquals(cached, value.Mesh)) {
        layers = new MapLayers(value.Mesh);
        cached = value.Mesh;
      }
      map = value;
      modPreferences.Sync(directory,value.Live,active);
      inventory = value.Live.inventory_tracker;
      if (inventory != null)
        inventory.Validate();
      var player = new HeightPoint(value.Live.player.position);
      follower.Update(layers, player);
      if (lastUpdate != value.Live.update) {
        if (trail.Count > 0) {
          var p = trail[trail.Count - 1];
          if (Math.Abs(p.X - player.X) + Math.Abs(p.Z - player.Z) > 1000)
            trail.Clear();
        }
        trail.Add(player);
        if (trail.Count > 256)
          trail.RemoveAt(0);
        lastUpdate = value.Live.update;
      }
      if (explorer.Running && !Paused) {
        var command = explorer.Tick(value, NavigationExplorer.Clock);
        if (command.Stop)
          Stop();
        if (command.Confirm)
          NavigationRoute.ConfirmTransition(directory, value.Live, ++nonce);
        if (command.Route != null)
          route = command.Route;
        aiStatus = explorer.Status;
      }
      if (explorer.Busy) {
        route = explorer.ActiveRoute;
        aiStatus = explorer.Status;
      }
      explorer.ObserveIdle(value, NavigationExplorer.Clock);
      if (explorer.Running || explorer.Busy)
        explorer.Save(directory);
      status = (Paused ? "PAUSED" : "LIVE") + "  /  Room " + value.Live.level +
               "  /  " + value.Live.exits.Length + " exits  /  " +
               value.Live.markers.Length + " items  /  " +
               value.Live.npcs.Length + " characters";
    } catch (Exception e) {
      if (!Expected(e))
        throw;
      map = null;
      inventory = null;
      cached = null;
      layers = null;
      status = e.Message;
      if (explorer.Running) {
        explorer.MissingMap(NavigationExplorer.Clock);
        aiStatus = explorer.Status;
      }
    }
  }
  private float Center {
    get { return mode == 1 ? height : follower.Height; }
  }
  private void Polygon(List<Triangle> target, HeightPoint[] p, uint tint) {
    for (int i = 1; i + 1 < p.Length; i++)
      target.Add(
          new Triangle { A = p[0], B = p[i], C = p[i + 1], Color = tint });
  }
  private bool Visible(float y) {
    return mode == 2 || other ||
           (y >= Center - slice / 2 && y <= Center + slice / 2);
  }
  private void AddMarker(List<Marker> target, uint id, float[] p, int shape,
                         uint tint, string label, string details, string kind,
                         string action) {
    if (!Visible(p[1]))
      return;
    if (mode != 2 && (p[1] < Center - slice / 2 || p[1] > Center + slice / 2))
      tint = (tint & 0xffffff00) | 110;
    // Prefer enriched interaction metadata when an actor also has a collision
    // box.
    var marker = new Marker { Id = id,      Position = p,   Shape = shape,
                              Color = tint, Label = label,  Details = details,
                              Kind = kind,  Action = action };
    int existing =
        target.FindIndex(delegate(Marker item) { return item.Id == id; });
    if (existing < 0)
      target.Add(marker);
    else
      target[existing] = marker;
  }
  private void Publish() {
    var triangles = new List<Triangle>();
    var lines = new List<Segment>();
    var markers = new List<Marker>();
    float lx = 0, lz = 0, hx = 1, hz = 1;
    if (map != null && layers != null) {
      lx = lz = Single.MaxValue;
      hx = hz = Single.MinValue;
      foreach (var p in map.Mesh.vertices) {
        lx = Math.Min(lx, p[0]);
        hx = Math.Max(hx, p[0]);
        lz = Math.Min(lz, p[2]);
        hz = Math.Max(hz, p[2]);
      }
      float low = Center - slice / 2, high = Center + slice / 2;
      if (mode != 2 && other)
        foreach (var floor in layers.Floors)
          if (floor.Low < low || floor.High > high)
            Polygon(triangles, floor.Points, 0x41536a55);
      foreach (var patch in layers.Patches) {
        if (mode != 2 && (patch.Surface.High < low || patch.Surface.Low > high))
          continue;
        var p = mode == 2 ? patch.Surface.Points
                          : MapLayers.Clip(patch.Surface.Points, low, high);
        Polygon(triangles, p, Tint(patch.Color));
      }
      if (collision && map.Live.collision != null && map.Live.collision.known)
        foreach (var m in map.Live.collision.models) {
          if (NavigationCollision.SameActor(m.address, map.Live.player.address))
            continue;
          float a = mode == 0 ? Center + NavigationCollision.FootClearance
                              : low,
                b = mode == 0 ? Center + NavigationCollision.BodyHeight : high;
          bool overlaps = mode == 2 || m.Overlaps(a, b);
          if (!overlaps && !other)
            continue;
          var p = new[] { new HeightPoint(m.lower[0], m.upper[1], m.lower[2]),
                          new HeightPoint(m.upper[0], m.upper[1], m.lower[2]),
                          new HeightPoint(m.upper[0], m.upper[1], m.upper[2]),
                          new HeightPoint(m.lower[0], m.upper[1], m.upper[2]) };
          uint tint = overlaps ? 0xe2a251bb : 0x8691a855;
          Polygon(triangles, p, (tint & 0xffffff00) | 40);
          for (int i = 0; i < 4; i++)
            lines.Add(new Segment { A = p[i], B = p[(i + 1) % 4], Width = 1.3f,
                                    Color = tint });
          var actor = NavigationCollision.Actor(map.Live, m.address);
          if (actor != null)
            AddMarker(markers, actor.address, actor.position, 4, tint,
                      actor.Name, NavigationCollision.Details(map.Live, actor),
                      "entity", "");
        }
      if (origins)
        foreach (var actor in map.Live.actors)
          if (!NavigationCollision.SameActor(actor.address,
                                             map.Live.player.address))
            AddMarker(markers, actor.address, actor.position, 4, 0xa7b0c2ff,
                      actor.Name, NavigationCollision.Details(map.Live, actor),
                      "entity", "");
      for (int i = 1; i < trail.Count; i++) {
        var a = trail[i - 1];
        var b = trail[i];
        if (mode != 2) {
          if ((a.Y < low && b.Y < low) || (a.Y > high && b.Y > high))
            continue;
          if (Math.Abs(b.Y - a.Y) > .0001f) {
            float ta = (low - a.Y) / (b.Y - a.Y),
                  tb = (high - a.Y) / (b.Y - a.Y);
            var old = a;
            a = HeightPoint.Lerp(old, b, Math.Max(0, Math.Min(ta, tb)));
            b = HeightPoint.Lerp(old, b, Math.Min(1, Math.Max(ta, tb)));
          }
        }
        lines.Add(
            new Segment { A = a, B = b, Width = 1.8f, Color = 0x6fd3ee99 });
      }
      var enriched = new HashSet<uint>();
      if (map.Live.progression != null)
        foreach (var n in map.Live.progression.nodes) {
          enriched.Add(n.address);
          uint c = n.kind == "exit"     ? 0xe9d16bff
                   : n.kind == "tribal" ? 0xe8ecf4ff
                   : n.kind == "npc"    ? 0x5b8defff
                   : n.kind == "gate"   ? 0xb58bfaff
                   : n.kind == "key"    ? 0xd19bebff
                                        : 0x5fd08aff;
          if (n.status == "owned" || n.status == "opened" ||
              n.status == "activated" || n.status == "inactive_alternative")
            c = 0x6f7a9199;
          AddMarker(markers, n.address, n.position,
                    n.kind == "npc" || n.kind == "tribal" ? 2
                    : n.kind == "exit"                    ? 0
                                                          : 1,
                    c, n.Caption, n.Details, n.kind, n.action);
        }
      foreach (var group in new[] { map.Live.exits, map.Live.markers,
                                    map.Live.npcs })
        foreach (var n in group)
          if (!enriched.Contains(n.address))
            AddMarker(markers, n.address, n.position,
                      n.kind == "exit"                        ? 0
                      : n.kind == "npc" || n.kind == "tribal" ? 2
                                                              : 1,
                      n.kind == "exit" ? 0xe9d16bff : 0x5fd08aff, n.label,
                      n.label, n.kind, "");
      // Enemy markers are live telemetry, independent of diagnostic overlays.
      foreach (var actor in map.Live.actors)
        if (actor.LiveEnemy && !NavigationCollision.SameActor(actor.address, map.Live.player.address))
          AddMarker(markers, actor.address, actor.position, 2, 0xef6b73ff,
                    "Enemy: " + actor.Name,
                    actor.Name + "\nHealth: " + actor.health +
                    "\nPosition: X " + actor.position[0].ToString("0.0") +
                    "  Y " + actor.position[1].ToString("0.0") +
                    "  Z " + actor.position[2].ToString("0.0"),
                    "enemy", "");
      if (route != null && route.Level == map.Live.level &&
          route.Generation == map.Live.generation) {
        var previous = new HeightPoint(map.Live.player.position);
        foreach (var p in route.Points) {
          lines.Add(new Segment { A = previous, B = p, Width = 2.5f,
                                  Color = 0xffc872ff });
          previous = p;
        }
      }
      AddMarker(markers, 0, map.Live.player.position, 3, 0x6fd3eeff, "Player",
                "", "player", "");
    }
    string file = Path.Combine(output, "snapshot.bin"), tmp = file + ".tmp";
    using (var stream = File.Create(tmp)) using (var w =
                                                     new BinaryWriter(stream)) {
      w.Write(0x32544c4a);
      w.Write(++sequence);
      w.Write(active ? 1 : 0);
      w.Write(active && Paused ? 1 : 0);
      w.Write(map != null ? 1 : 0);
      w.Write(map == null ? 0 : map.Live.level);
      w.Write(map == null ? 0 : map.Live.generation);
      w.Write(map == null ? 0 : map.Live.update);
      w.Write(map != null && map.Live.navigation_enabled &&
                      map.Live.clearing_active
                  ? 1
                  : 0);
      Text(w, aiStatus);
      Text(w, NavigationExplorer.Clock < noticeUntil ? notice : status);
      Text(w, map == null || map.Live.progression == null
                  ? "Not loaded yet"
                  : map.Live.progression.inventory.Summary);
      Text(w, artStatus);
      w.Write(artReady ? 1 : 0);
      bool known = active && inventory != null && inventory.known;
      w.Write(known ? inventory.current : -1);
      w.Write(known ? 1 : 0);
      for (int i = 0; i < 3; i++) {
        var c = known ? Array.Find(inventory.characters,
                                   delegate(CharacterInventory item) {
                                     return item.id == i;
                                   })
                      : null;
        w.Write(c == null ? 0 : c.weapons);
        w.Write(c == null ? 0 : Bits(c.items));
      }
      w.Write(known ? Bits(inventory.shared) : 0);
      w.Write(lx);
      w.Write(lz);
      w.Write(hx);
      w.Write(hz);
      w.Write(Center);
      w.Write(layers == null ? 0 : layers.Low);
      w.Write(layers == null ? 1 : layers.High);
      w.Write(triangles.Count);
      foreach (var t in triangles) {
        Point(w, t.A);
        Point(w, t.B);
        Point(w, t.C);
        w.Write(t.Color);
      }
      w.Write(lines.Count);
      foreach (var l in lines) {
        Point(w, l.A);
        Point(w, l.B);
        w.Write(l.Width);
        w.Write(l.Color);
      }
      w.Write(markers.Count);
      foreach (var m in markers) {
        w.Write(m.Id);
        foreach (float v in m.Position)
          w.Write(v);
        w.Write(m.Shape);
        w.Write(m.Color);
        Text(w, m.Label);
        Text(w, m.Details);
        Text(w, m.Kind);
        Text(w, m.Action);
      }
      var mods = modPreferences.Values;
      w.Write(1 | (modPreferences.Failed ? 16 : 0) |
              (mods.Warp ? 2 : 0) | (mods.Health ? 4 : 0) | (mods.Kill ? 8 : 0));
      w.Write(modPreferences.ErrorVersion);
    }
    if (File.Exists(file))
      File.Replace(tmp, file, null);
    else
      File.Move(tmp, file);
  }
}
}
