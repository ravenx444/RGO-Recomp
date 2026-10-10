using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace JfgLauncher
{
    internal static class LauncherTests
    {
        private static int checks;
        private static void Check(bool pass, string description)
        {
            if (!pass) throw new Exception(description);
            checks++;
        }
        private static void Reject(Action action, string description)
        {
            try { action(); } catch (InvalidDataException) { checks++; return; }
            throw new Exception(description);
        }
        private static byte[] Fixture()
        {
            byte[] data = new byte[32];
            data[0] = 0x80; data[1] = 0x37; data[2] = 0x12; data[3] = 0x40;
            return data;
        }
        private static byte[] Pe()
        {
            byte[] data = new byte[128];
            data[0] = (byte)'M'; data[1] = (byte)'Z'; data[60] = 80;
            data[80] = (byte)'P'; data[81] = (byte)'E'; data[84] = 0x64; data[85] = 0x86;
            return data;
        }

        private static void InventoryAssetTests(string directory) {
            byte[] rgba16=new byte[48];rgba16[0]=4;rgba16[1]=2;rgba16[2]=1;
            // Red then transparent; on the odd row blue is stored in the swapped half.
            rgba16[32]=0xf8;rgba16[33]=1;rgba16[44]=0;rgba16[45]=0x3f;
            using(var image=InventoryImages.Decode(rgba16)) {
                Check(image.Width==4&&image.Height==2,"texture dimensions changed");
                Check(image.GetPixel(0,0).R==255&&image.GetPixel(0,0).A==255,"RGBA5551 red decode failed");
                Check(image.GetPixel(1,0).A==0,"RGBA5551 transparency lost");
                Check(image.GetPixel(0,1).B==255&&image.GetPixel(0,1).A==255,"odd texture row not unswizzled");
            }
            byte[] rgba32=new byte[64];rgba32[0]=4;rgba32[1]=2;rgba32[2]=0;
            rgba32[56]=25;rgba32[57]=80;rgba32[58]=160;rgba32[59]=123;
            using(var image=InventoryImages.Decode(rgba32))Check(image.GetPixel(0,1)==Color.FromArgb(123,25,80,160),"RGBA32 swizzle or alpha decode failed");
            byte[] compressed;
            using(var output=new MemoryStream()) {
                byte[] header=new byte[37];header[25]=1;header[32]=(byte)rgba16.Length;output.Write(header,0,header.Length);
                using(var deflate=new System.IO.Compression.DeflateStream(output,System.IO.Compression.CompressionMode.Compress,true))deflate.Write(rgba16,0,rgba16.Length);
                compressed=output.ToArray();
            }
            using(var image=InventoryImages.Decode(compressed))Check(image.GetPixel(0,1).B==255,"compressed texture was not decoded");
            Reject(delegate{InventoryImages.Decode(new byte[4]);},"short texture header accepted");
            var invalid=(byte[])rgba16.Clone();invalid[2]=7;Reject(delegate{InventoryImages.Decode(invalid);},"unsupported palette texture accepted");
            invalid=(byte[])rgba16.Clone();invalid[0]=128;Reject(delegate{InventoryImages.Decode(invalid);},"truncated pixels accepted");
            invalid=(byte[])compressed.Clone();invalid[35]=127;Reject(delegate{InventoryImages.Decode(invalid);},"unbounded decompression allocation accepted");
            invalid=(byte[])compressed.Clone();invalid[32]++;Reject(delegate{InventoryImages.Decode(invalid);},"incorrect expanded length accepted");
            string wrongRom=Path.Combine(directory,"wrong-art-rom.z64");File.WriteAllBytes(wrongRom,Fixture());
            Reject(delegate{InventoryImages.Load(wrongRom);},"art cache bypassed ROM verification");
            var tracker=new InventoryTracker{known=true,current=0,shared=new bool[12],characters=new[]{
                new CharacterInventory{id=0,weapons=1,items=new bool[27]},new CharacterInventory{id=1,weapons=2,items=new bool[28]},new CharacterInventory{id=2,weapons=4,items=new bool[28]}}};
            tracker.Validate();
            using(var window=new InventoryWindow(directory,"")) {
                window.Apply(tracker,true);Check(window.SelectedCharacter==0,"inventory did not follow Vela enum 0");
                tracker.current=2;window.Apply(tracker,false);Check(window.SelectedCharacter==0,"stale inventory changed selected character");
                window.Apply(tracker,true);Check(window.SelectedCharacter==2,"live inventory did not follow Lupus");
                var flags=System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic;
                var follow=(CheckBox)typeof(InventoryWindow).GetField("follow",flags).GetValue(window);follow.Checked=false;
                tracker.current=1;window.Apply(tracker,true);Check(window.SelectedCharacter==2,"manual character selection was overridden");
                var canvas=(InventoryCanvas)typeof(InventoryWindow).GetField("canvas",flags).GetValue(window);
                window.Apply(tracker,false);Check(canvas.Value==null&&canvas.Shared==null,"stale ownership persisted after telemetry stopped");
                window.Apply(null,false);Check(canvas.Value==null&&canvas.Shared==null,"stale ownership persisted after unavailable telemetry");
                follow.Checked=true;window.Apply(tracker,true);Check(window.SelectedCharacter==1,"follow did not resume for Juno enum 1");
                string badInventory=Path.Combine(directory,"bad-inventory");Directory.CreateDirectory(badInventory);
                File.WriteAllText(Path.Combine(badInventory,"live.json"),"{\"schema\":2}");
                window.BindDirectory(badInventory);Check(canvas.Value==null,"invalid schema retained ownership instead of waiting");
            }
        }

        private static void MenuLiveRegressionTests(string directory) {
            var flags=System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance;
            using(var map=new MapCanvas()) {
                map.Size=new Size(800,600);
                var project=typeof(MapCanvas).GetMethod("Project",flags,null,new[]{typeof(HeightPoint)},null);
                var point=new HeightPoint(new float[]{.2f,0,.1f});
                PointF anchor=(PointF)project.Invoke(map,new object[]{point});
                var wheel=typeof(MapCanvas).GetMethod("OnMouseWheel",flags);
                var cursor=new Point((int)anchor.X,(int)anchor.Y);
                // Use an exact integral projected anchor and verify actual wheel dispatch.
                point=new HeightPoint(new float[]{(cursor.X-(map.Width-30)/2f)/525f,0,(cursor.Y-(map.Height-25)/2f)/525f});
                wheel.Invoke(map,new object[]{new MouseEventArgs(MouseButtons.None,0,cursor.X,cursor.Y,120)});
                PointF after=(PointF)project.Invoke(map,new object[]{point});
                Check(Math.Abs(after.X-cursor.X)<.01f&&Math.Abs(after.Y-cursor.Y)<.01f,"wheel zoom moved point under cursor");
                for(int i=0;i<80;i++)map.ZoomAt(1.2f,cursor);
                after=(PointF)project.Invoke(map,new object[]{point});
                Check(Math.Abs(after.X-cursor.X)<.01f&&Math.Abs(after.Y-cursor.Y)<.01f,"zoom clamp drifted cursor anchor");
                for(int i=0;i<100;i++)map.ZoomAt(1/1.2f,cursor);
                after=(PointF)project.Invoke(map,new object[]{point});
                Check(Math.Abs(after.X-cursor.X)<.01f&&Math.Abs(after.Y-cursor.Y)<.01f,"zoom out drifted cursor anchor");
            }
            string profile=Path.Combine(directory,"session-follow","default");Directory.CreateDirectory(profile);
            string older=Path.Combine(LocalSetup.NavigationProfile(profile),"maps","aaa"),newer=Path.Combine(LocalSetup.NavigationProfile(profile),"maps","zzz");
            Directory.CreateDirectory(older);Directory.CreateDirectory(newer);
            Check(FrontendBridge.MapDirectory(profile)==Path.Combine(profile,"no-active-live-session"),"inactive tool resurrected saved map data");
            string marker=Path.Combine(profile,"frontend-map-session.txt");File.WriteAllText(marker,older);
            Check(FrontendBridge.MapDirectory(profile)!=older,"stopped process left a stale active map");
            using(var process=Process.GetCurrentProcess())InventorySession.Record(profile,process);
            Check(FrontendBridge.MapDirectory(profile)==older,"active map session was replaced by cached latest folder");
            File.WriteAllText(marker,"disabled");
            Check(FrontendBridge.MapDirectory(profile)!=newer,"ordinary run exposed an old map session");
            File.Delete(marker);
            Check(InventorySession.IsActive(profile),"live process identity not recognized");
            File.WriteAllText(Path.Combine(profile,"inventory-session.txt"),Process.GetCurrentProcess().Id+"\n1");
            Check(!InventorySession.IsActive(profile),"reused PID masqueraded as a live game");
        }

        private sealed class PublishedMarker {
            internal uint Id,Color;
            internal int Shape;
            internal float X,Y,Z;
            internal string Label,Details,Kind;
        }
        private static string SnapshotText(BinaryReader r) {
            return Encoding.UTF8.GetString(r.ReadBytes(r.ReadInt32()));
        }
        private static System.Collections.Generic.List<PublishedMarker> PublishedMarkers(string path) {
            var markers=new System.Collections.Generic.List<PublishedMarker>();
            using(var r=new BinaryReader(File.OpenRead(path))) {
                Check(r.ReadUInt32()==0x32544c4a,"Live tool snapshot signature");
                r.BaseStream.Position=44;
                for(int i=0;i<4;i++)SnapshotText(r);
                r.BaseStream.Position+=68;
                int triangles=r.ReadInt32();r.BaseStream.Position+=triangles*28;
                int lines=r.ReadInt32();r.BaseStream.Position+=lines*24;
                int count=r.ReadInt32();
                for(int i=0;i<count;i++) {
                    var m=new PublishedMarker {Id=r.ReadUInt32(),X=r.ReadSingle(),Y=r.ReadSingle(),Z=r.ReadSingle(),Shape=r.ReadInt32(),Color=r.ReadUInt32(),Label=SnapshotText(r),Details=SnapshotText(r),Kind=SnapshotText(r)};
                    SnapshotText(r);markers.Add(m);
                }
            }
            return markers;
        }
        private static void MapModPreferenceTests(string directory) {
            string profile=Path.Combine(directory,"mod-preferences");Directory.CreateDirectory(profile);
            var prefs=new MapModPreferences(profile);
            Check(!prefs.Failed&&!prefs.Values.Health,"Waiting for a game must not fail Mods");
            prefs.Choose(new MapMods{Health=true,Warp=true});
            prefs.Sync("",null,false);
            Check(!prefs.Failed&&prefs.Values.Health&&prefs.Values.Warp&&!prefs.Values.Kill,"Pre-game choice lost");
            var reopened=new MapModPreferences(profile);
            Check(reopened.Values.Health&&reopened.Values.Warp,"Reopening map lost preferences");
            // Launch applies saved choices even with no map service running.
            var launch=LocalSetup.LiveToolsStartInfo(Path.Combine(profile,"jfg-native-boot.exe"),Path.Combine(profile,"game.z64"),profile);
            string session=launch.EnvironmentVariables["JFG_LIVE_OUTPUT"];
            var applied=MapMods.Load(session,null);
            Check(applied.Warp&&applied.Health&&!applied.Kill,"Launch did not apply pre-game preferences");
            var live=new MapLive{mods_available=true,mod_warp_exits=true,mod_infinite_health=true,update=100};
            prefs.Sync(session,live,true);Check(!prefs.Failed,"Compatible runtime was rejected");
            live.mods_available=false;prefs.Sync(session,live,true);
            Check(prefs.Failed&&prefs.ErrorVersion==1,"Confirmed unsupported runtime did not report failure");
            prefs.Sync(session,live,true);Check(prefs.ErrorVersion==1,"Same failed connection repeated notification");
            prefs.Sync("",null,false);Check(!prefs.Failed,"Disconnected session kept controls locked");
            string next=Path.Combine(profile,"new-session");Directory.CreateDirectory(next);live.mods_available=true;
            prefs.Sync(next,live,true);Check(!prefs.Failed&&prefs.Values.Health,"New session did not recover saved choices");
            prefs.Choose(new MapMods{Kill=true});prefs.Sync(next,live,true);
            for(int i=0;i<20;i++)prefs.Sync(next,live,true);
            Check(!prefs.Failed,"Paused updates incorrectly timed out Mods");
            live.update+=18;prefs.Sync(next,live,true);
            Check(prefs.Failed&&prefs.ErrorVersion==2,"Ignored settings were not detected after game updates");
            prefs.Choose(new MapMods());live.mod_warp_exits=live.mod_infinite_health=false;prefs.Sync(next,live,true);
            Check(!prefs.Failed&&!new MapModPreferences(profile).Values.Kill,"Restore defaults did not recover and persist");
            string broken=Path.Combine(directory,"unwritable-mods");Directory.CreateDirectory(broken);File.WriteAllText(Path.Combine(broken,"map-settings"),"block directory");
            var failed=new MapModPreferences(broken);failed.Choose(new MapMods{Health=true});
            Check(failed.Failed&&failed.ErrorVersion==1&&!failed.Values.Health,"Write failure looked like a successful toggle");
            string invalid=Path.Combine(directory,"invalid-mods");Directory.CreateDirectory(Path.Combine(invalid,"map-settings"));File.WriteAllText(Path.Combine(invalid,"map-settings/mods.txt"),"broken");
            Check(new MapModPreferences(invalid).Failed,"Corrupt saved Mods were ignored");
            // Native bridge can save choices with no running game and no map.
            string bridgeProfile=Path.Combine(directory,"pre-game-bridge");
            var bridge=new NativeLiveTools(bridgeProfile,IntPtr.Zero,false);
            var flags=System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic;
            string output=(string)typeof(NativeLiveTools).GetField("output",flags).GetValue(bridge);
            File.WriteAllText(Path.Combine(output,"command-1.txt"),"health-mod 1");
            typeof(NativeLiveTools).GetMethod("Commands",flags).Invoke(bridge,null);
            Check(new MapModPreferences(bridgeProfile).Values.Health,"Native pre-game toggle was blocked");
        }
        private static void MapModsTests(string directory) {
            string root=Path.Combine(directory,"mods-test");Directory.CreateDirectory(root);
            var exit=new MapMarker{address=0x80120000,position=new float[]{10,20,30}};
            var live=new MapLive{level=27,generation=3,timestamp_ms=NavigationExplorer.Clock,mods_available=true,gameplay_active=true,exits=new[]{exit}};
            var map=new MapSnapshot{Live=live};
            var mods=MapMods.Load(root,live);
            Check(!mods.Warp&&!mods.Health&&!mods.Kill,"Ordinary session Mods must default off");
            Reject(delegate{mods.WarpTo(root,map,exit.address,1,false);},"Disabled warp accepted");
            mods.Health=true;mods.Save(root);mods=MapMods.Load(root,live);
            Check(mods.Health&&!mods.Warp&&!mods.Kill,"Health toggle enabled unrelated Mods");
            mods.Warp=true;mods.Kill=true;mods.Save(root);mods=MapMods.Load(root,live);
            Check(mods.Warp&&mods.Health&&mods.Kill,"Mods did not survive reopening");
            mods.WarpTo(root,map,exit.address,17,false);
            string text=File.ReadAllText(Path.Combine(root,"warp-exit.txt"));
            Check(text.StartsWith("JFGWARP1 27 3 17 ")&&text.TrimEnd().EndsWith(exit.address.ToString()),"Warp did not bind to current room and exit");
            Reject(delegate{mods.WarpTo(root,map,0x80120004,18,false);},"Non-exit warp accepted");
            live.timestamp_ms=NavigationExplorer.Clock-6000;
            Reject(delegate{mods.WarpTo(root,map,exit.address,19,false);},"Stale map accepted");
            mods.WarpTo(root,map,exit.address,20,true);
            live.scripted_camera=true;Reject(delegate{mods.WarpTo(root,map,exit.address,21,true);},"Scripted scene warp accepted");
            live.scripted_camera=false;live.timestamp_ms=NavigationExplorer.Clock;
            live.mods_available=false;Reject(delegate{mods.WarpTo(root,map,exit.address,22,false);},"Old runtime accepted Mods");live.mods_available=true;
            // Exercise the actual native bridge commands, including exact uint exit IDs.
            var service=new NativeLiveTools(Path.Combine(root,"profile"),IntPtr.Zero,false);
            var flags=System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic;var type=typeof(NativeLiveTools);
            type.GetField("map",flags).SetValue(service,map);type.GetField("active",flags).SetValue(service,true);type.GetField("directory",flags).SetValue(service,root);
            var commands=type.GetMethod("Commands",flags);string output=(string)type.GetField("output",flags).GetValue(service);
            Action<string> send=delegate(string command){File.WriteAllText(Path.Combine(output,"command-1.txt"),command);commands.Invoke(service,null);};
            send("mods-off");mods=MapMods.Load(root,live);Check(!mods.Warp&&!mods.Health&&!mods.Kill,"Restore defaults did not disable Mods");
            send("health-mod 1");send("kill-mod 1");mods=MapMods.Load(root,live);Check(mods.Health&&mods.Kill&&!mods.Warp,"Bridge toggles coupled");
            send("warp-mod 1");send("warp "+exit.address);Check(File.ReadAllText(Path.Combine(root,"warp-exit.txt"))!=text,"Bridge did not dispatch warp");
            string before=File.ReadAllText(Path.Combine(root,"warp-exit.txt"));send("warp-mod 0");send("warp "+exit.address);
            Check(File.ReadAllText(Path.Combine(root,"warp-exit.txt"))==before,"Bridge dispatched disabled warp");
            send("mods-off");Check(File.ReadAllText(Path.Combine(root,"mods.txt")).Trim()=="JFGMODS1 0 0 0","Defaults not persisted");
        }
        private static void EnemyMarkerTests(string directory) {
            var enemy=new MapActor {address=0x80001000,behavior=24,name="Squad member",position=new float[]{20,0,40},hostile_known=true,hostile=true,health=200};
            var dead=new MapActor {address=0x80002000,behavior=24,position=new float[]{50,0,60},hostile_known=true,hostile=true,health=0};
            var friendly=new MapActor {address=0x80003000,behavior=24,position=new float[]{30,0,60},hostile_known=true,health=100};
            var unknown=new MapActor {address=0x80004000,behavior=24,position=new float[]{40,0,60},health=100};
            var playerActor=new MapActor {address=0x80005000,position=new float[]{50,0,50},hostile_known=true,hostile=true,health=100};
            var mesh=new MapGeometry {vertices=new[]{new float[]{0,0,0},new float[]{0,0,100},new float[]{100,0,0}},triangles=new[]{new MapFace{v=new[]{0,1,2}}}};
            var live=new MapLive {level=1,generation=1,update=1,player=new MapPlayer{address=playerActor.address,position=playerActor.position},actors=new[]{enemy,dead,friendly,unknown,playerActor},exits=new MapMarker[0],markers=new MapMarker[0],npcs=new MapMarker[0]};
            var service=new NativeLiveTools(Path.Combine(directory,"enemy-map"),IntPtr.Zero,false);
            var flags=System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic;
            var type=typeof(NativeLiveTools);
            type.GetField("map",flags).SetValue(service,new MapSnapshot{Mesh=mesh,Live=live});
            type.GetField("layers",flags).SetValue(service,new MapLayers(mesh));
            type.GetField("active",flags).SetValue(service,true);
            type.GetField("collision",flags).SetValue(service,false);
            type.GetField("origins",flags).SetValue(service,false);
            var publish=type.GetMethod("Publish",flags);
            string output=Path.Combine((string)type.GetField("output",flags).GetValue(service),"snapshot.bin");
            publish.Invoke(service,null);var first=PublishedMarkers(output);
            Check(first.Count==2,"Enemies must appear with overlays off; dead, friendly, unknown and player actors must not");
            var marker=first.Find(delegate(PublishedMarker m){return m.Kind=="enemy";});
            Check(marker!=null&&marker.Id==enemy.address&&marker.Shape==2&&marker.Color==0xef6b73ff,"Enemy marker identity/style");
            Check(marker.Label=="Enemy: Squad member"&&marker.Details.Contains("Health: 200"),"Enemy inspector health");
            enemy.position=new float[]{75,0,25};live.update++;
            publish.Invoke(service,null);var moved=PublishedMarkers(output).Find(delegate(PublishedMarker m){return m.Kind=="enemy";});
            Check(moved!=null&&moved.X==75&&moved.Z==25,"Enemy markers must follow current positions");
            type.GetField("origins",flags).SetValue(service,true);
            publish.Invoke(service,null);var overlays=PublishedMarkers(output);
            Check(overlays.FindAll(delegate(PublishedMarker m){return m.Id==enemy.address;}).Count==1,"Origin overlay must not duplicate enemy markers");
            type.GetField("origins",flags).SetValue(service,false);
            enemy.health=0;publish.Invoke(service,null);
            Check(PublishedMarkers(output).Count==1,"Dead enemy marker must disappear on the next update");
            enemy.health=100;live.actors=new MapActor[0];publish.Invoke(service,null);
            Check(PublishedMarkers(output).Count==1,"Despawned enemy marker must disappear on the next update");
            using(var json=new MemoryStream(Encoding.UTF8.GetBytes("{\"address\":2147487744,\"behavior\":24,\"position\":[1,2,3]}"))) {
                var old=(MapActor)new System.Runtime.Serialization.Json.DataContractJsonSerializer(typeof(MapActor)).ReadObject(json);
                Check(!old.LiveEnemy,"Older telemetry must not guess enemy identity");
            }
        }
        private static void InventoryModelTests(string directory) {
            byte[] ia=new byte[48];ia[0]=8;ia[1]=2;ia[2]=5;ia[44]=0x4f;
            using(var image=InventoryImages.Decode(ia))Check(image.GetPixel(0,1)==Color.FromArgb(255,68,68,68),"IA8 texture intensity or row order lost");
            byte[] model=new byte[240];model[19]=3;model[21]=1;model[23]=1;
            model[27]=136;model[31]=168;model[35]=208;model[39]=136;
            model[136]=255;model[152]=255;model[161]=1;
            // Three independently authored positions in the game's packed layout.
            model[168]=255;model[169]=246;model[170]=255;model[171]=246;
            model[178]=0;model[179]=10;model[180]=255;model[181]=246;
            model[188]=0;model[189]=0;model[190]=0;model[191]=10;
            model[209]=0;model[210]=1;model[211]=2;
            using(var image=InventoryModel.Render(model,delegate(int id){throw new Exception("Untextured fixture loaded a texture");})) {
                int visible=0;for(int y=0;y<128;y++)for(int x=0;x<128;x++)if(image.GetPixel(x,y).A>0)visible++;
                Check(visible>2000&&visible<8000,"model triangle projection or rasterization failed");
                Check(image.GetPixel(0,0).A==0,"thumbnail background is not transparent");
            }
            var invalid=(byte[])model.Clone();invalid[211]=4;
            Reject(delegate{InventoryModel.Render(invalid,delegate(int id){return null;});},"model vertex index escaped bounds");
            invalid=(byte[])model.Clone();invalid[19]=255;invalid[18]=255;
            Reject(delegate{InventoryModel.Render(invalid,delegate(int id){return null;});},"oversized model was accepted");
            string wrong=Path.Combine(directory,"wrong-checksum-art.z64");
            using(var stream=File.Create(wrong)){stream.Write(Fixture(),0,32);stream.SetLength(LocalSetup.RomSize);}
            Reject(delegate{InventoryImages.Load(wrong);},"art cache accepted a different ROM checksum");
        }

        private static void LayerTests()
        {
            MapGeometry mesh = new MapGeometry {
                vertices = new float[][] { new float[]{0,0,0}, new float[]{0,0,100}, new float[]{100,0,0},
                    new float[]{0,100,0}, new float[]{0,100,100}, new float[]{100,100,0},
                    new float[]{0,200,0}, new float[]{100,200,0}, new float[]{0,200,100},
                    new float[]{0,0,0}, new float[]{0,100,0}, new float[]{0,0,100} },
                triangles = new MapFace[] { new MapFace {v=new int[]{0,1,2}}, new MapFace {v=new int[]{3,4,5}},
                    new MapFace {v=new int[]{6,7,8}}, new MapFace {v=new int[]{9,10,11}} }
            };
            MapLayers layers = new MapLayers(mesh);
            Check(layers.Floors.Count == 2, "walls and downward ceilings must not be filled as floors");
            float ground;
            Check(layers.Support(new HeightPoint(20,2,20), out ground) && ground == 0, "upper floor incorrectly obscures lower support");
            Check(layers.Support(new HeightPoint(20,102,20), out ground) && ground == 100, "upper floor support missing");
            Check(!layers.Support(new HeightPoint(200,2,200), out ground), "support extends outside triangle");
            FloorFollower follow = new FloorFollower();
            follow.Update(layers, new HeightPoint(20,2,20));
            follow.Update(layers, new HeightPoint(20,150,20));
            Check(follow.Height == 0 && !follow.Grounded, "jump switches to a floor far below airborne player");
            follow.Update(layers, new HeightPoint(20,102,20));
            Check(follow.Height == 100 && follow.Grounded, "landing does not switch floor");
            Check(MapLayers.Clip(layers.Floors[0].Points, -10, 10).Length == 3 &&
                MapLayers.Clip(layers.Floors[1].Points, -10, 10).Length == 0, "lower slice merges stacked floors");
            Check(MapLayers.Clip(layers.Floors[0].Points, 90, 110).Length == 0 &&
                MapLayers.Clip(layers.Floors[1].Points, 90, 110).Length == 3, "upper slice merges stacked floors");
            HeightPoint[] ramp = {new HeightPoint(0,0,0),new HeightPoint(0,0,100),new HeightPoint(100,100,0)};
            HeightPoint[] clipped = MapLayers.Clip(ramp,40,60);
            Check(clipped.Length == 4, "partial ramp should remain a quadrilateral");
            bool lower=false,upper=false; double area=0;
            for(int i=0;i<clipped.Length;i++) {
                HeightPoint p=clipped[i],q=clipped[(i+1)%clipped.Length];
                Check(p.Y >= 39.999f && p.Y <= 60.001f && Math.Abs(p.X-p.Y)<.001f, "ramp intersection misplaced");
                lower |= Math.Abs(p.Y-40)<.001f; upper |= Math.Abs(p.Y-60)<.001f;
                area += p.X*q.Z-q.X*p.Z;
            }
            Check(lower && upper && Math.Abs(Math.Abs(area)/2-1000)<.01, "ramp cross-section area incorrect");
            Check(layers.Low == 0 && layers.High == 100, "height scale includes ceilings or changes with slice");
            Check(layers.Patches.Count == 2 && layers.Patches[0].Color != layers.Patches[1].Color, "stacked floor colors indistinguishable");
            mesh.triangles[0].normal = new float[]{0,-1,0};
            Check(new MapLayers(mesh).Floors.Count == 1, "exported normal ignored");
        }

        [STAThread]
        private static int Main(string[] args)
        {
            if(args.Length>0 && args[0]=="--frontend-version") {
                string fixture=Environment.GetEnvironmentVariable("JFG_FRONTEND_TEST_PROTOCOL");
                if(fixture=="hang")System.Threading.Thread.Sleep(5000);
                Console.WriteLine(fixture=="ok"?"jfg-frontend-1":"old-runtime");
                return 0;
            }
            if (args.Length > 0 && args[0] == "--echo")
            {
                for (int i = 1; i < args.Length; i++) Console.WriteLine(Convert.ToBase64String(Encoding.UTF8.GetBytes(args[i])));
                return 0;
            }
            if (args.Length > 0 && args[0] == "--crash-fixture")
            {
                Console.Error.WriteLine("PRIVATE-PATH-CANARY local ROM and save contents");
                Console.Error.Write(new string('x', 200000));
                Console.Error.WriteLine();
                Console.Error.WriteLine("JFG-SUPPORT native=running");
                Console.Error.WriteLine("native boot child rejected: exit=0xc0000005");
                return 17;
            }
            string directory = Path.Combine(Path.GetTempPath(), "jfg-launcher-fixture-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(directory);
            try
            {
                byte[] fixture = Fixture();
                string hash = LocalSetup.Digest(new MemoryStream(fixture));
                LocalSetup.ValidateRom(new MemoryStream(fixture), fixture.Length, hash);
                checks++;
                Reject(delegate { LocalSetup.ValidateRom(new MemoryStream(fixture), 33, hash); }, "wrong size accepted");
                Reject(delegate { LocalSetup.ValidateRom(new MemoryStream(fixture), fixture.Length, new string('0', 40)); }, "wrong hash accepted");
                fixture[0] = 0x37;
                Reject(delegate { LocalSetup.ValidateRom(new MemoryStream(fixture), fixture.Length, hash); }, "wrong byte order accepted");
                string wrongRom = Path.Combine(directory, "synthetic.z64");
                File.WriteAllBytes(wrongRom, Fixture());
                Reject(delegate { using (LocalSetup.OpenVerifiedRom(wrongRom)) { } }, "production validation accepted synthetic ROM");
                Reject(delegate { LocalSetup.FullPath("bad\npath"); }, "newline path accepted");
                Reject(delegate { LocalSetup.FullPath(@"\\server\share\file"); }, "network path accepted");
                Reject(delegate { LocalSetup.ValidateRuntime(Path.Combine(directory, "jfg.exe")); }, "host shell accepted");
                string game = Path.Combine(directory, "jfg-native-boot.exe");
                File.WriteAllBytes(game, Pe());
                Reject(delegate { LocalSetup.ValidateRuntime(game); }, "missing libraries accepted");
                foreach (string name in new string[] {"SDL2.dll", "dxcompiler.dll", "dxil.dll"}) File.WriteAllBytes(Path.Combine(directory, name), Pe());
                Check(LocalSetup.ValidateRuntime(game) == game, "complete structural build rejected");
                byte[] badPe = Pe(); badPe[85] = 0;
                File.WriteAllBytes(Path.Combine(directory, "SDL2.dll"), badPe);
                Reject(delegate { LocalSetup.ValidateRuntime(game); }, "non-x64 library accepted");
                File.WriteAllBytes(game, new byte[2]);
                Reject(delegate { LocalSetup.ValidateRuntime(game); }, "truncated executable accepted");
                Check(LocalSetup.PreferredRuntime(directory, "saved runtime") == "saved runtime", "invalid paired build accepted");
                File.WriteAllBytes(game, Pe());
                File.WriteAllBytes(Path.Combine(directory, "SDL2.dll"), Pe());
                Check(LocalSetup.PreferredRuntime(directory, "saved runtime") == game, "paired native build not selected");
                Settings settings = new Settings { RuntimePath = "Synthetic build & Unicode \u00e9", RomPath = "Selected synthetic ROM" };
                LocalSetup.SaveSettings(directory, settings);
                Settings loaded = LocalSetup.LoadSettings(directory);
                Check(loaded.RuntimePath == settings.RuntimePath && loaded.RomPath == settings.RomPath, "settings round trip failed");
                settings.RomPath = "Updated selection";
                LocalSetup.SaveSettings(directory, settings);
                Check(LocalSetup.LoadSettings(directory).RomPath == settings.RomPath, "atomic replacement failed");
                string flash = Path.Combine(directory, "jfg.flash");
                File.WriteAllText(flash, "synthetic saved data");
                LocalSetup.SaveSettings(directory, settings);
                Check(File.ReadAllText(flash) == "synthetic saved data", "settings overwrote save data");
                File.WriteAllText(Path.Combine(directory, "launcher.json"), "broken");
                Check(LocalSetup.LoadSettings(directory).RuntimePath == "", "malformed settings not recovered");
                Environment.SetEnvironmentVariable("JFG_PHASE9_INPUT_RECORD", "must-not-inherit");
                ProcessStartInfo start = LocalSetup.StartInfo(game, wrongRom, directory);
                Check(!start.UseShellExecute && start.CreateNoWindow, "launch uses shell or console");
                Check(!start.EnvironmentVariables.ContainsKey("JFG_PHASE9_INPUT_RECORD"), "diagnostic environment inherited");
                Check(start.EnvironmentVariables["JFG_MASTER_VOLUME_CONFIG"] == AudioPreferences.FileName(directory), "audio preferences not forwarded");
                Check(start.EnvironmentVariables["JFG_VIDEO_CONFIG"] == VideoPreferences.FileName(directory), "video preferences not forwarded");
                AudioPreferences audio = AudioPreferences.Load(directory);
                Check(audio.Volume == 100 && !audio.Muted, "default audio settings changed");
                audio.Volume = 37; audio.Muted = true; audio.Save(directory);
                audio = AudioPreferences.Load(directory);
                Check(audio.Volume == 37 && audio.Muted, "mute or retained volume did not persist");
                audio.Muted = false; audio.Save(directory);
                Check(AudioPreferences.Load(directory).Volume == 37 && !AudioPreferences.Load(directory).Muted, "unmute lost prior volume");
                Check(AudioPreferences.Parse("version=1\r\nvolume=0\r\nmuted=0\r\n").Volume == 0, "zero volume or CRLF rejected");
                foreach (string invalidAudio in new string[] {
                    "version=1\nvolume=101\nmuted=0\n", "version=1\nvolume=-1\nmuted=0\n",
                    "version=1\nvolume=01\nmuted=0\n", "version=1\nvolume=50\nmuted=2\n",
                    "version=1\nvolume=50\nmuted=0\nextra", new string('x',129) })
                    Reject(delegate { AudioPreferences.Parse(invalidAudio); }, "invalid audio settings accepted");

                // A saved shortcut must enter the window-owning launcher, retaining menus.
                string shortcutPath = Path.Combine(directory, "Quick play.lnk");
                string frontendExe = System.Reflection.Assembly.GetExecutingAssembly().Location;
                Reject(delegate { FrontendBridge.SaveDirectShortcut(shortcutPath, directory, null); }, "shortcut fell back to a headless helper");
                FrontendBridge.SaveDirectShortcut(shortcutPath, directory, frontendExe);
                object shortcutShell = null, savedShortcut = null;
                try {
                    var shellType = Type.GetTypeFromProgID("WScript.Shell", true);
                    shortcutShell = Activator.CreateInstance(shellType);
                    savedShortcut = shellType.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shortcutShell, new object[] { shortcutPath });
                    var shortcutType = savedShortcut.GetType();
                    Func<string, string> property = delegate(string key) { return (string)shortcutType.InvokeMember(key, System.Reflection.BindingFlags.GetProperty, null, savedShortcut, null); };
                    Check(String.Equals(property("TargetPath"), frontendExe, StringComparison.OrdinalIgnoreCase), "shortcut bypasses frontend");
                    Check(property("Arguments") == "--play --profile " + LocalSetup.Quote(LocalSetup.FullPath(directory)), "shortcut lost quick-play or profile arguments");
                    Check(String.Equals(property("WorkingDirectory"), Path.GetDirectoryName(frontendExe), StringComparison.OrdinalIgnoreCase), "shortcut has incorrect working directory");
                } finally {
                    if(savedShortcut != null) System.Runtime.InteropServices.Marshal.FinalReleaseComObject(savedShortcut);
                    if(shortcutShell != null) System.Runtime.InteropServices.Marshal.FinalReleaseComObject(shortcutShell);
                }
                string replayPath = Path.Combine(directory, "cutscene input.txt");
                string progressPath = Path.Combine(directory, "cutscene progress.json");
                File.WriteAllText(replayPath, "jfg-phase8-input-v2\n");
                Environment.SetEnvironmentVariable("JFG_PHASE8_INPUT_REPLAY", "must-not-inherit");
                var replayStart = LocalSetup.StartInfo(game, wrongRom, directory);
                Check(!replayStart.EnvironmentVariables.ContainsKey("JFG_PHASE8_INPUT_REPLAY"), "inherited replay activated ordinary play");
                Environment.SetEnvironmentVariable("JFG_PHASE8_INPUT_REPLAY", null);
                FrontendBridge.ApplyReplayOptions(replayStart, new[] {"--frontend-worker", "play", directory, "0", "--input-replay", replayPath, "--progress-output", progressPath});
                Check(replayStart.EnvironmentVariables["JFG_PHASE8_INPUT_REPLAY"] == replayPath && replayStart.EnvironmentVariables["JFG_PHASE8_PROGRESS"] == progressPath, "explicit replay options lost");
                Reject(delegate { FrontendBridge.ApplyReplayOptions(replayStart, new[] {"--frontend-worker", "play", directory, "0", "--input-replay", replayPath}); }, "duplicate replay accepted");
                Reject(delegate { FrontendBridge.ApplyReplayOptions(LocalSetup.StartInfo(game, wrongRom, directory), new[] {"--frontend-worker", "play", directory, "0", "--input-replay", replayPath + ".missing"}); }, "missing input replay accepted");
                Reject(delegate { FrontendBridge.ApplyReplayOptions(LocalSetup.StartInfo(game, wrongRom, directory), new[] {"--frontend-worker", "play", directory, "0", "--input-replay"}); }, "missing replay argument accepted");
                Reject(delegate { FrontendBridge.ApplyReplayOptions(LocalSetup.StartInfo(game, wrongRom, directory), new[] {"--frontend-worker", "setup", directory, "0", "--input-replay", replayPath}); }, "setup received replay options");

                Check(start.WorkingDirectory == directory && start.Arguments.EndsWith(" --play"), "incorrect game launch plan");
                Check(start.Arguments.Contains(LocalSetup.Quote(flash)), "save path not isolated");
                Check(!LocalSetup.FriendlyError(new IOException("PRIVATE-PATH-CANARY")).Contains("PRIVATE-PATH-CANARY"), "exception discloses path");
                string[] samples = {"", "plain", "spaces here", "ampersand & dollar $ percent %", "a\"b", @"trailing\", "\u00e9 unicode", "back\\\"quote"};
                ProcessStartInfo echo = new ProcessStartInfo();
                echo.FileName = System.Reflection.Assembly.GetExecutingAssembly().Location;
                echo.UseShellExecute = false; echo.CreateNoWindow = true; echo.RedirectStandardOutput = true;
                echo.Arguments = "--echo";
                foreach (string sample in samples) echo.Arguments += " " + LocalSetup.Quote(sample);
                using (Process child = Process.Start(echo))
                {
                    foreach (string sample in samples)
                        Check(child.StandardOutput.ReadLine() == Convert.ToBase64String(Encoding.UTF8.GetBytes(sample)), "Windows argument round trip failed");
                    child.WaitForExit();
                    Check(child.ExitCode == 0, "echo process failed");
                }
                ControllerProfile controller = new ControllerProfile();
                string repo = LocalSetup.FindSourceRoot(AppDomain.CurrentDomain.BaseDirectory);
                Check(controller.Encode() == File.ReadAllText(Path.Combine(repo, "tests", "fixtures", "controller-default.ini")).Replace("\r\n", "\n"), "C# and native mapping schema differ");
                controller.Device = 2; controller.Bindings[3] = 3; controller.Deadzone = 18000;
                controller.Save(directory);
                ControllerProfile recovered = ControllerProfile.Load(directory);
                Check(recovered.Device == 2 && recovered.Bindings[3] == 3 && recovered.Deadzone == 18000, "controller settings did not persist");
                Check(LocalSetup.StartInfo(game, wrongRom, directory).EnvironmentVariables["JFG_CONTROLLER_CONFIG"] == ControllerProfile.FileName(directory), "controller mapping not forwarded to game");
                string oldProtocol=Environment.GetEnvironmentVariable("JFG_FRONTEND_TEST_PROTOCOL");
                try {
                    string fixtureExe=System.Reflection.Assembly.GetExecutingAssembly().Location;
                    Environment.SetEnvironmentVariable("JFG_FRONTEND_TEST_PROTOCOL","ok");
                    FrontendBridge.CheckRuntimeProtocol(fixtureExe,3000);Check(true,"compatible game protocol rejected");
                    Environment.SetEnvironmentVariable("JFG_FRONTEND_TEST_PROTOCOL","old");
                    Reject(delegate{FrontendBridge.CheckRuntimeProtocol(fixtureExe,3000);},"old runtime protocol accepted");
                    Environment.SetEnvironmentVariable("JFG_FRONTEND_TEST_PROTOCOL","hang");
                    var protocolWatch=Stopwatch.StartNew();
                    Reject(delegate{FrontendBridge.CheckRuntimeProtocol(fixtureExe,250);},"hung runtime protocol accepted");
                    Check(protocolWatch.ElapsedMilliseconds<3000,"runtime compatibility check hung");
                } finally {Environment.SetEnvironmentVariable("JFG_FRONTEND_TEST_PROTOCOL",oldProtocol);}
                string portRoot=Path.Combine(directory,"four-ports");
                for(int p=1;p<4;++p)Check(ControllerProfile.Load(portRoot,p).Device==-1,"new multiplayer port must discover unassigned controllers");
                for(int p=0;p<4;++p) {
                    var perPort=new ControllerProfile{Device=p,Deadzone=1000+p*100};
                    perPort.Bindings[0]=p;perPort.Save(portRoot,p);
                }
                for(int p=0;p<4;++p) {
                    var perPort=ControllerProfile.Load(portRoot,p);
                    Check(perPort.Device==p && perPort.Bindings[0]==p && perPort.Deadzone==1000+p*100,"one player's save changed another port");
                }
                Reject(delegate { new ControllerProfile{Device=2}.ValidateAssignment(portRoot,0); },"physical device shared by two players");
                new ControllerProfile{Device=-3}.Save(portRoot,1);
                Reject(delegate { new ControllerProfile{Device=-3}.ValidateAssignment(portRoot,3); },"keyboard shared by two players");
                new ControllerProfile{Device=-2}.Save(portRoot,1);
                Check(ControllerProfile.Load(portRoot,1).Device==-2 && ControllerProfile.Load(portRoot,0).Device==0,"disconnect changed the wrong player");
                Check(ControllerProfile.Parse(new ControllerProfile{Device=-3}.Encode()).Device==-3,"keyboard profile rejected");
                Reject(delegate { ControllerProfile.Parse(new ControllerProfile{Device=-4}.Encode()); },"invalid device sentinel accepted");

                string normalProfile = Path.Combine(directory, "profiles", "default");
                Directory.CreateDirectory(normalProfile);
                File.WriteAllText(Path.Combine(normalProfile, "jfg.flash"), "normal campaign");
                File.WriteAllText(Path.Combine(normalProfile, "controller-1.pak"), "normal pak");
                controller.Save(normalProfile);
                ProcessStartInfo modStart = LocalSetup.NavigationStartInfo(game, wrongRom, normalProfile);
                string modProfile = LocalSetup.NavigationProfile(normalProfile);
                Check(modProfile != normalProfile && modStart.Arguments.Contains(LocalSetup.Quote(Path.Combine(modProfile, "jfg.flash"))), "mod shares normal saves");
                Check(File.ReadAllText(Path.Combine(modProfile, "jfg.flash")) == "normal campaign", "mod did not copy initial progress");
                Check(modStart.EnvironmentVariables["JFG_NAVIGATION_MOD"] == "1", "mod not enabled explicitly");
                Check(modStart.EnvironmentVariables["JFG_MASTER_VOLUME_CONFIG"] == AudioPreferences.FileName(normalProfile), "mod must share normal volume settings");
                Check(modStart.EnvironmentVariables["JFG_VIDEO_CONFIG"] == VideoPreferences.FileName(normalProfile), "mod must share normal video settings");
                Check(modStart.EnvironmentVariables["JFG_MOD_OUTPUT"].StartsWith(Path.Combine(modProfile, "maps")), "map export outside mod profile");
                Check(modStart.EnvironmentVariables["JFG_CONTROLLER_CONFIG"] == ControllerProfile.FileName(modProfile), "mod controller profile missing");
                File.WriteAllText(Path.Combine(modProfile, "jfg.flash"), "mod progress");
                ProcessStartInfo secondModStart = LocalSetup.NavigationStartInfo(game, wrongRom, normalProfile);
                Check(File.ReadAllText(Path.Combine(modProfile, "jfg.flash")) == "mod progress", "mod launch overwrites mod progress");
                Check(File.ReadAllText(Path.Combine(normalProfile, "jfg.flash")) == "normal campaign", "mod changed normal campaign");
                Check(secondModStart.EnvironmentVariables["JFG_MOD_OUTPUT"] != modStart.EnvironmentVariables["JFG_MOD_OUTPUT"], "export sessions share stale maps");
                ProcessStartInfo normalStart = LocalSetup.StartInfo(game, wrongRom, normalProfile);
                var liveStart=LocalSetup.LiveToolsStartInfo(game,wrongRom,normalProfile);
                Check(liveStart.Arguments==normalStart.Arguments,"live tools changed the normal save profile");
                Check(liveStart.EnvironmentVariables.ContainsKey("JFG_LIVE_OUTPUT")&&!liveStart.EnvironmentVariables.ContainsKey("JFG_NAVIGATION_MOD")&&!liveStart.EnvironmentVariables.ContainsKey("JFG_MOD_OUTPUT"),"live tools enabled navigation cheats");
                Check(LocalSetup.LiveToolsStartInfo(game,wrongRom,normalProfile).EnvironmentVariables["JFG_LIVE_OUTPUT"]!=liveStart.EnvironmentVariables["JFG_LIVE_OUTPUT"],"live tools reused old session exports");
                Check(!normalStart.EnvironmentVariables.ContainsKey("JFG_NAVIGATION_MOD") && !normalStart.EnvironmentVariables.ContainsKey("JFG_MOD_OUTPUT"), "mod enabled for normal launch");
                Reject(delegate { ControllerProfile.Parse(controller.Encode() + "map0=27\n"); }, "duplicate mapping accepted");
                Reject(delegate { ControllerProfile.Parse(controller.Encode().Replace("device=2", "device=4")); }, "bad controller port accepted");
                Reject(delegate { ControllerProfile.Parse(controller.Encode().Replace("map3=3", "map3=99")); }, "bad binding accepted");
                Reject(delegate { ControllerProfile.Parse(controller.Encode().Replace("deadzone=18000", "deadzone=99999")); }, "bad dead zone accepted");
                bool[] buttons = new bool[15]; int[] axes = new int[6];
                buttons[6] = true; Check(ControllerInput.Pressed(buttons, axes) == 6, "Start capture failed");
                buttons[6] = false; axes[3] = -24000; Check(ControllerInput.Pressed(buttons, axes) == 22, "stick direction capture failed");
                string reportRoot = Path.Combine(directory, "reports");
                SupportSession support = new SupportSession(reportRoot, "launch");
                support.Write("mod=navigation-enabled");
                support.Write("mod=PRIVATE-PATH-CANARY");
                support.Error(new IOException("PRIVATE-PATH-CANARY"));
                ProcessStartInfo crashPlan = new ProcessStartInfo(System.Reflection.Assembly.GetExecutingAssembly().Location, "--crash-fixture") {
                    UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
                using (Process child = Process.Start(crashPlan)) {
                    var stdout = support.Drain(child.StandardOutput); var stderr = support.Drain(child.StandardError);
                    child.WaitForExit(); System.Threading.Tasks.Task.WaitAll(stdout, stderr); support.Exit(child.ExitCode);
                }
                support.Finish();
                string diagnosticFixture = Environment.GetEnvironmentVariable("JFG_SUPPORT_TEST_FIXTURES");
                if (!String.IsNullOrEmpty(diagnosticFixture)) {
                    foreach (string name in new string[] { "crash", "hang" }) {
                        string evidence = File.ReadAllText(Path.Combine(diagnosticFixture, "support-fixture-" + name + ".log"));
                        File.WriteAllText(Path.Combine(support.DirectoryPath, name + ".log"), evidence + "PRIVATE-MEMORY-CANARY\n");
                    }
                }
                File.WriteAllText(support.NativePath, "native=running\nfailure=runlink/guest-overlay-load\nPRIVATE-PATH-CANARY\nnative_exception=0xc0000005\n");
                File.WriteAllText(Path.Combine(support.DirectoryPath, "jfg.flash"), "SAVE-CONTENTS-CANARY");
                File.WriteAllText(Path.Combine(support.DirectoryPath, "game.z64"), "ROM-CONTENTS-CANARY");
                string assetCache=Path.Combine(support.DirectoryPath,"asset-cache");Directory.CreateDirectory(assetCache);
                File.WriteAllText(Path.Combine(assetCache,"weapon-0.png"),"ROM-ART-CANARY");
                File.WriteAllText(Path.Combine(assetCache,"manifest.json"),"ROM-MANIFEST-CANARY");
                File.WriteAllText(Path.Combine(support.DirectoryPath,"item-200.png"),"ROM-THUMBNAIL-CANARY");
                string zipPath = SupportSession.Export(reportRoot, Path.Combine(directory, "exports"));
                using (FileStream file = File.OpenRead(zipPath))
                using (System.IO.Compression.ZipArchive zip = new System.IO.Compression.ZipArchive(file)) {
                    Check(zip.Entries.Count == 9, "unexpected support archive members");
                    string combined = "";
                    foreach (var entry in zip.Entries) using (StreamReader reader = new StreamReader(entry.Open())) combined += reader.ReadToEnd();
                    Check(!combined.Contains("CANARY") && !combined.Contains(directory), "private contents leaked into support report");
                    Check(combined.Contains("exit=0x00000011") && combined.Contains("native_exit=0xc0000005") && combined.Contains("failure=runlink/guest-overlay-load"), "crash evidence lost");
                    Check(combined.Contains("mod=navigation-enabled"), "mod context lost from support report");
                    if (!String.IsNullOrEmpty(diagnosticFixture)) {
                        Check(combined.Contains("snapshot=crash-v1") && combined.Contains("snapshot=hang-v1") && combined.Contains("/jfg-support-fixture.exe/"), "native stack evidence lost during ZIP export");
                    }
                    Check(combined.Contains("native_exception=0xc0000005") && combined.Contains("omitted="), "exception or omitted count lost");
                }
                SupportSession later = new SupportSession(reportRoot, "launch"); later.Exit(0);
                Check(SupportSession.Sessions(reportRoot).Find(delegate(SupportChoice c) { return c.Path == support.DirectoryPath; }).Failed, "failed session lost after successful launch");
                string oldZip = SupportSession.Export(reportRoot, Path.Combine(directory, "exports"));
                using (FileStream file = File.OpenRead(oldZip)) using (System.IO.Compression.ZipArchive zip = new System.IO.Compression.ZipArchive(file))
                using (StreamReader reader = new StreamReader(zip.GetEntry("launcher.log").Open())) Check(reader.ReadToEnd().Contains("exit=0x00000011"), "default export did not select failed session");
                Reject(delegate { SupportSession.ExportSelected(reportRoot, directory, Path.Combine(directory, "exports")); }, "export accepted unrelated directory");
                support.SetupLine("PRIVATE-PATH-CANARY fatal error C1083: missing header");
                support.SetupLine("error 0x80370102 PRIVATE-PATH-CANARY");
                string safe = SupportSession.SanitizeFile(Path.Combine(support.DirectoryPath, "launcher.log"));
                Check(safe.Contains("compiler_error=c1083") && safe.Contains("setup_error=virtualization-unavailable") && !safe.Contains("CANARY"), "setup error projection failed");
                support.SystemDetails();
                Check(SupportSession.SanitizeFile(Path.Combine(support.DirectoryPath, "system.log")).Contains("architecture=x64"), "system diagnostics missing");
                Check(!support.RuntimeDetails(game, directory), "legacy executable incorrectly advertises capture");
                string runtimeHash = SupportSession.Hash(game);
                File.WriteAllText(game + ".support", "build_runtime_sha256=" + runtimeHash + "\nbuild_source=" + new string('b', 40) + "\nPRIVATE-PATH-CANARY\n");
                support.RuntimeDetails(game, directory);
                Check(SupportSession.SanitizeFile(Path.Combine(support.DirectoryPath, "build.log")).Contains("build_source=" + new string('b', 40)), "independent game build identity missing");
                File.WriteAllText(game + ".support", "build_runtime_sha256=" + new string('0', 64) + "\nbuild_source=" + new string('c', 40) + "\n");
                support.RuntimeDetails(game, directory);
                Check(SupportSession.SanitizeFile(Path.Combine(support.DirectoryPath, "build.log")) == "build_identity=unavailable\n", "stale build manifest trusted");
                for (int i = 0; i < 6000; ++i) support.Write("native=running");
                Check(new FileInfo(Path.Combine(support.DirectoryPath, "launcher.log")).Length <= SupportSession.MaximumBytes, "support log grew without bound");
                for (int i = 0; i < 12; ++i) new SupportSession(reportRoot, "launch");
                Check(Directory.GetDirectories(reportRoot).Length == 11, "report retention should preserve unknown files but prune owned sessions");
                using (ControllerWindow controllerWindow = new ControllerWindow(directory)) {
                    controllerWindow.StartPosition = FormStartPosition.Manual; controllerWindow.Location = new Point(-32000, -32000);
                    controllerWindow.ShowInTaskbar = false; controllerWindow.Show(); Application.DoEvents();
                    using (Bitmap preview = new Bitmap(controllerWindow.Width, controllerWindow.Height)) {
                        controllerWindow.DrawToBitmap(preview, new Rectangle(Point.Empty, controllerWindow.Size));
                        if (args.Length == 1) preview.Save(Path.Combine(Path.GetDirectoryName(args[0]), "controller-preview.png"), System.Drawing.Imaging.ImageFormat.Png);
                    }
                    Check(controllerWindow.Controls.Count != 0, "controller UI did not render"); controllerWindow.Close();
                }
                Application.EnableVisualStyles();
                string sourceFixture = Path.Combine(directory, "source with spaces");
                Directory.CreateDirectory(Path.Combine(sourceFixture, "scripts"));
                File.WriteAllText(Path.Combine(sourceFixture, "CMakeLists.txt"), "# fixture");
                File.WriteAllText(Path.Combine(sourceFixture, "scripts", "build_from_rom.py"), "# fixture");
                string nested = Path.Combine(sourceFixture, "build", "launcher");
                Directory.CreateDirectory(nested);
                Check(LocalSetup.FindSourceRoot(nested) == sourceFixture, "source root discovery failed");
                ProcessStartInfo buildPlan = LocalSetup.BuildStartInfo(sourceFixture, Path.Combine(directory, "local rom.z64"));
                Check(!buildPlan.UseShellExecute && buildPlan.CreateNoWindow && buildPlan.RedirectStandardOutput && buildPlan.RedirectStandardError,
                    "build process must use redirected argument-safe execution");
                Check(buildPlan.WorkingDirectory == sourceFixture && buildPlan.Arguments.Contains(" --rom "), "build inputs missing");
                Reject(delegate { LocalSetup.BuildStartInfo(directory, "rom.z64"); }, "unrelated source folder accepted");
                ProcessStartInfo sourceSetup = LocalSetup.SourceSetupStartInfo(sourceFixture, Path.Combine(directory, "local rom.z64"));
                Check(sourceSetup.WorkingDirectory == sourceFixture && sourceSetup.Arguments.StartsWith("-u ") &&
                    sourceSetup.Arguments.EndsWith(" --no-frontend") && sourceSetup.Arguments.Contains(" --rom "),
                    "source setup must rebuild only the game from the checkout");
                Reject(delegate { LocalSetup.SourceSetupStartInfo(directory, "rom.z64"); }, "unrelated source folder accepted for setup");
                string setupFixture = Path.Combine(directory, "setup with spaces");
                ProcessStartInfo setupPlan = FirstRun.StartInfo(wrongRom, setupFixture);
                Check(!setupPlan.UseShellExecute && setupPlan.RedirectStandardError && setupPlan.RedirectStandardOutput,
                    "setup must use redirected argument-safe execution");
                Check(setupPlan.Arguments.Contains(LocalSetup.Quote(wrongRom)) &&
                    setupPlan.Arguments.Contains(BuildInfo.SourceCommit), "setup lost ROM or immutable revision");
                Check(System.Text.RegularExpressions.Regex.IsMatch(BuildInfo.SourceCommit, "^[0-9a-f]{40}$"), "source is not pinned");
                string installer = Path.Combine(setupFixture, "Setup.ps1");
                File.WriteAllText(installer, "synthetic tamper");
                FirstRun.StartInfo(wrongRom, setupFixture);
                Check(Convert.ToBase64String(File.ReadAllBytes(installer)) == Convert.ToBase64String(FirstRun.SetupScript()),
                    "setup resource was not restored from the executable");
                Reject(delegate { FirstRun.StartInfo("bad\npath", setupFixture); }, "setup accepted malformed ROM path");
                MapGeometry routeMesh = new MapGeometry { vertices = new float[][] {new float[]{0,0,0},new float[]{200,0,0},new float[]{0,0,200},new float[]{200,0,200}},
                    triangles = new MapFace[] {new MapFace {v=new int[]{0,2,1},normal=new float[]{0,1,0}},new MapFace {v=new int[]{1,2,3},normal=new float[]{0,1,0}}} };
                MapSnapshot routeSnapshot = new MapSnapshot {Mesh=routeMesh,Live=new MapLive {collision=MapCollision.Empty(),level=35,generation=2,player=new MapPlayer {position=new float[]{20,0,20}}}};
                NavigationRoute route = NavigationRoute.Plan(routeSnapshot,new float[]{180,0,180});
                Check(route.Points.Count==1 && route.Points[0].X==180 && route.Points[0].Z==180,"open floor route was not straightened");
                route.Send(directory,123,false,false);
                Check(File.ReadAllText(Path.Combine(directory,"ai-command.txt")).StartsWith("JFGNAV3 35 2 123 "),"route command room identity missing");
                route.Send(directory,124,false,true);
                Check(File.ReadAllLines(Path.Combine(directory,"ai-command.txt")).Length==1,"stop command retained waypoints");
                Reject(delegate { NavigationRoute.Plan(routeSnapshot,new float[]{500,0,500}); },"unmapped exit accepted");
                MapGeometry stepMesh = new MapGeometry {vertices=new float[][] {new float[]{0,0,0},new float[]{200,0,0},new float[]{0,0,200},new float[]{200,20,0},new float[]{0,20,200},new float[]{200,20,200}},triangles=new MapFace[] {new MapFace {v=new int[]{0,2,1},normal=new float[]{0,1,0}},new MapFace {v=new int[]{3,4,5},normal=new float[]{0,1,0}}}};
                MapSnapshot stepSnapshot=new MapSnapshot {Mesh=stepMesh,Live=routeSnapshot.Live};
                NavigationRoute stepRoute=NavigationRoute.Plan(stepSnapshot,new float[]{180,20,180});
                Check(stepRoute.Points[stepRoute.Points.Count-1].Y==20,"small step not connected");
                stepMesh.vertices[3][1]=stepMesh.vertices[4][1]=stepMesh.vertices[5][1]=80;
                Reject(delegate {NavigationRoute.Plan(stepSnapshot,new float[]{180,80,180});},"tall step silently bridged");
                routeMesh.vertices = new float[][] {new float[]{0,0,0},new float[]{200,0,0},new float[]{0,0,200},new float[]{0,500,0},new float[]{200,500,0},new float[]{0,500,200}};
                routeMesh.triangles = new MapFace[] {new MapFace {v=new int[]{0,2,1},normal=new float[]{0,1,0}},new MapFace {v=new int[]{3,5,4},normal=new float[]{0,1,0}}};
                Reject(delegate { NavigationRoute.Plan(routeSnapshot,new float[]{20,500,20}); },"disconnected stacked floors merged");
                LayerTests();
                string mapDirectory = Path.Combine(directory, "map");
                Directory.CreateDirectory(mapDirectory);
                string meshFixture = "{\"schema\":1,\"level\":21,\"generation\":2,\"vertices\":[[0,0,0],[0,0,100],[100,0,0]],\"triangles\":[{\"v\":[0,1,2]}]}";
                long mapNow = (long)(DateTime.UtcNow - new DateTime(1970,1,1,0,0,0,DateTimeKind.Utc)).TotalMilliseconds;
                string liveFixture = "{\"schema\":1,\"level\":21,\"generation\":2,\"timestamp_ms\":" + mapNow +
                    ",\"update\":1,\"mesh_ready\":true,\"clearing_active\":true,\"player\":{\"position\":[20,0,20]},\"exits\":[{\"position\":[80,0,10],\"destination_code\":123}],\"markers\":[{\"position\":[10,0,80],\"kind\":\"chest\",\"label\":\"Chest\"}]}";
                File.WriteAllText(Path.Combine(mapDirectory, "mesh.json"), meshFixture);
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture);
                MapSnapshot map = MapSnapshot.Load(mapDirectory, null);
                Check(map.IsLive && map.Live.markers.Length == 1 && map.Live.exits.Length == 1, "live map markers missing");
                Check(map.Live.npcs.Length == 0, "legacy maps should have no NPC markers");
                string npcFixture = ",\"npcs\":[{\"position\":[25,0,40],\"kind\":\"npc\",\"label\":\"NPC: Guide\"},{\"position\":[40,0,65],\"kind\":\"tribal\",\"label\":\"Tribal\"}]}";
                liveFixture = liveFixture.Substring(0, liveFixture.Length - 1) + npcFixture;
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture);
                map = MapSnapshot.Load(mapDirectory, map.Mesh);
                Check(map.Live.npcs.Length == 2 && map.Live.npcs[1].kind == "tribal", "NPC and Tribal markers missing");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture.Replace("[25,0,40]", "[25,0]"));
                Reject(delegate { MapSnapshot.Load(mapDirectory, map.Mesh); }, "malformed NPC position accepted");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture.Replace("\"kind\":\"npc\"", "\"kind\":\"enemy\""));
                Reject(delegate { MapSnapshot.Load(mapDirectory, map.Mesh); }, "unknown NPC category accepted");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture.Replace("NPC: Guide", new string('x', 81)));
                Reject(delegate { MapSnapshot.Load(mapDirectory, map.Mesh); }, "oversized NPC label accepted");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture);
                Check(Object.ReferenceEquals(map.Mesh, MapSnapshot.Load(mapDirectory, map.Mesh).Mesh), "map cache not reused");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture.Replace("\"generation\":2", "\"generation\":3"));
                Reject(delegate { MapSnapshot.Load(mapDirectory, map.Mesh); }, "map mixed two room generations");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture.Replace(mapNow.ToString(), "1"));
                Check(!MapSnapshot.Load(mapDirectory, map.Mesh).IsLive, "stale map presented as live");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), liveFixture);
                File.WriteAllText(Path.Combine(mapDirectory, "mesh.json"), meshFixture.Replace("[0,1,2]", "[0,1,9]"));
                Reject(delegate { MapSnapshot.Load(mapDirectory, null); }, "invalid map face accepted");
                File.WriteAllText(Path.Combine(mapDirectory, "mesh.json"), meshFixture);
                File.WriteAllText(Path.Combine(mapDirectory, "mesh.json"), meshFixture.Replace("\"v\":[0,1,2]", "\"v\":[0,1,2],\"normal\":[0,1]"));
                Reject(delegate { MapSnapshot.Load(mapDirectory, null); }, "malformed surface normal accepted");
                File.WriteAllText(Path.Combine(mapDirectory, "mesh.json"), meshFixture);
                string progressFixture = @"{""schema"":1,""inventory"":{""known"":true,""character"":1,""red_key"":false,""weapons_mask"":4},""nodes"":[{""address"":2148712448,""position"":[20,0,40],""kind"":""npc"",""label"":""Magnus: Red key"",""action"":""talk"",""status"":""available"",""requirement"":""Talk to Magnus"",""reward"":""Red key"",""requirement_known"":true,""reward_item"":1,""reward_weapon"":-1,""required_weapon"":-1,""spoken"":1,""traversal"":""unknown""}]}";
                string withProgress = liveFixture.Substring(0, liveFixture.Length - 1) + ",\"progression\":" + progressFixture + "}";
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), withProgress);
                map = MapSnapshot.Load(mapDirectory, map.Mesh);
                Check(map.Live.progression.nodes[0].spoken == 1 && map.Live.progression.nodes[0].status == "available", "spoken NPC incorrectly marked complete");
                Check(map.Live.progression.inventory.Summary.Contains("Red key: missing") && map.Live.progression.inventory.Summary.Contains("Machine gun: owned"), "inventory display incorrect");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), withProgress.Replace("\"known\":true", "\"known\":false"));
                Reject(delegate { MapSnapshot.Load(mapDirectory, map.Mesh); }, "unknown inventory accepted ownership");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), withProgress.Replace("\"traversal\":\"unknown\"", "\"traversal\":\"open\""));
                Reject(delegate { MapSnapshot.Load(mapDirectory, map.Mesh); }, "unsupported route status accepted");
                File.WriteAllText(Path.Combine(mapDirectory, "live.json"), withProgress);
                MapNpcOffer sampleOffer = new MapNpcOffer { id = "7:0/1:0", kind = "item", reward = "Crowbar", status = "blocked", scope = "any_character",
                    action = 5, item = 21, weapon = -1, flag = -1, destination = -1, consumed_items = new int[] {20},
                    conditions = new MapNpcCondition[] { new MapNpcCondition { domain = "prerequisite", id = 3, description = "Current character payment", state = "missing" } } };
                sampleOffer.Validate();
                Check(sampleOffer.Details.Contains("missing") && sampleOffer.Details.Contains("consumed"), "NPC trade requirement or consumption omitted");
                sampleOffer.conditions[0].state = "complete";
                Reject(delegate { sampleOffer.Validate(); }, "invalid NPC condition accepted");
                sampleOffer.conditions[0].state = "missing";
                sampleOffer.consumed_items = new int[] {27};
                Reject(delegate { sampleOffer.Validate(); }, "invalid NPC payment accepted");
                sampleOffer.consumed_items = new int[] {20};
                MapInteraction npcNode = map.Live.progression.nodes[0];
                npcNode.npc_catalog_known = true; npcNode.offers = new MapNpcOffer[] { sampleOffer };
                map.Live.progression.Validate();
                Check(npcNode.Details.Contains("Crowbar") && npcNode.Details.Contains("NPC offers"), "NPC offer not displayed in inspector");
                npcNode.offers = new MapNpcOffer[] { sampleOffer, sampleOffer };
                Reject(delegate { map.Live.progression.Validate(); }, "duplicate NPC offer accepted");
                npcNode.offers = new MapNpcOffer[] { sampleOffer }; npcNode.npc_catalog_known = false;
                Reject(delegate { map.Live.progression.Validate(); }, "unknown NPC catalog accepted rewards");
                npcNode.npc_catalog_known = true;
                npcNode.label = "NPC: Trader"; npcNode.reward = "Crowbar"; npcNode.status = "blocked";
                using (FileStream fixtureStream = File.Create(Path.Combine(mapDirectory, "live.json")))
                    new System.Runtime.Serialization.Json.DataContractJsonSerializer(typeof(MapLive)).WriteObject(fixtureStream, map.Live);
                map = MapSnapshot.Load(mapDirectory, map.Mesh);
                Check(map.Live.progression.nodes[0].offers[0].conditions[0].state == "missing", "NPC offers lost during JSON round trip");
                using (NavigationMapWindow mapWindow = new NavigationMapWindow(mapDirectory)) {
                    mapWindow.StartPosition = FormStartPosition.Manual; mapWindow.Location = new Point(-32000, -32000);
                    mapWindow.ShowInTaskbar = false; mapWindow.Show(); Application.DoEvents();
                    using (Bitmap preview = new Bitmap(mapWindow.Width, mapWindow.Height)) {
                        mapWindow.DrawToBitmap(preview, new Rectangle(Point.Empty, preview.Size));
                        preview.Save(Path.Combine(repo, "build", "launcher", "map-preview.png"));
                    }
                    mapWindow.Close(); checks++;
                }
                using (LauncherWindow window = new LauncherWindow())
                {
                    // Realize controls without putting a test window on the user's desktop.
                    window.StartPosition = FormStartPosition.Manual;
                    window.Location = new Point(-32000, -32000);
                    window.ShowInTaskbar = false;
                    window.Show();
                    Application.DoEvents();
                    using (Bitmap preview = new Bitmap(window.Width, window.Height))
                    {
                        window.DrawToBitmap(preview, new Rectangle(Point.Empty, window.Size));
                        System.Collections.Generic.HashSet<int> colors = new System.Collections.Generic.HashSet<int>();
                        for (int y = 40; y < preview.Height - 20; y += 4)
                            for (int x = 20; x < preview.Width - 20; x += 4)
                                colors.Add(preview.GetPixel(x, y).ToArgb());
                        Check(colors.Count > 10, "UI rendered an empty surface");
                        if (args.Length == 1) preview.Save(args[0], System.Drawing.Imaging.ImageFormat.Png);
                    }
                    Check(window.Text == "JFG Launcher Preview", "UI construction failed");
                    window.Close();
                }
                InventoryAssetTests(directory);
                InventoryModelTests(directory);
                MapModPreferenceTests(directory);
                MapModsTests(directory);
                EnemyMarkerTests(directory);
                MenuLiveRegressionTests(directory);
                checks += NavigationExplorerTests.Run(directory);
                checks += NavigationCollisionTests.Run(directory);
                checks += BoxJumpTests.Run();
                checks += AutonomousExplorerTests.Run();
                checks += UnifiedNavigationTests.Run(directory);
                Console.WriteLine("Launcher checks passed: " + checks);
                return 0;
            }
            catch (Exception error) { Console.Error.WriteLine(error.ToString()); return 1; }
            finally
            {
                // This path is an explicitly created disposable fixture, never a user profile.
                string full = Path.GetFullPath(directory);
                if (full.StartsWith(Path.GetFullPath(Path.GetTempPath()), StringComparison.OrdinalIgnoreCase) &&
                    Path.GetFileName(full).StartsWith("jfg-launcher-fixture-", StringComparison.Ordinal))
                    Directory.Delete(full, true);
            }
        }
    }
}
