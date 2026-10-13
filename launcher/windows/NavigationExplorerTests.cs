using System;
using System.Collections.Generic;
using System.IO;
using System.Drawing;
using System.Windows.Forms;
using System.Runtime.Serialization.Json;

namespace JfgLauncher {
    internal static class NavigationExplorerTests {
        private static int checks;
        private static void Check(bool value,string reason){if(!value)throw new Exception("Explorer: "+reason);++checks;}
        private static MapMarker Exit(int id,float x,float z) {
            return new MapMarker {address=(uint)id,destination_code=65000+id,position=new float[]{x,0,z},normal=new float[]{1,0,0}};
        }
        internal static MapSnapshot Room(uint level,long generation,params MapMarker[] exits) {
            return new MapSnapshot {
                Mesh=new MapGeometry {schema=1,level=level,generation=generation,vertices=new float[][] {new float[]{-1000,0,-1000},new float[]{1000,0,-1000},new float[]{-1000,0,1000},new float[]{1000,0,1000}},
                    triangles=new MapFace[] {new MapFace {v=new int[]{0,2,1},normal=new float[]{0,1,0}},new MapFace {v=new int[]{1,2,3},normal=new float[]{0,1,0}}}},
                Live=new MapLive {dialogue=new MapDialogue{rows=new MapDialogueChoice[0]},collision=MapCollision.Empty(),actors=new MapActor[0],schema=1,level=level,generation=generation,mesh_ready=true,clearing_active=true,timestamp_ms=100000,update=1,
                    player=new MapPlayer {position=new float[]{0,0,0}},exits=exits,markers=new MapMarker[0],npcs=new MapMarker[0],navigation_ai=new MapAi(),
                    progression=new MapProgression {schema=1,inventory=new MapInventory {known=true,character=0,red_key=false,weapons_mask=1},nodes=new MapInteraction[0]}}
            };
        }
        private static MapSnapshot DoorRoom() {
            MapSnapshot map=Room(107,1,Exit(108,600,0));
            map.Mesh.vertices=new float[][] {new float[]{-100,0,-70},new float[]{800,0,-70},new float[]{-100,0,70},new float[]{800,0,70}};
            map.Live.actors=new MapActor[]{new MapActor {address=1234,name="Lifting door",position=new float[]{440,0,0}}};
            map.Live.collision.models=new MapCollisionModel[]{new MapCollisionModel {address=1234,enabled=true,lower=new float[]{420,0,-70},upper=new float[]{460,150,70}}};
            map.Live.progression.nodes=new MapInteraction[]{new MapInteraction {address=1234,kind="gate",action="pass_door",position=new float[]{440,0,0}}};return map;
        }
        private static NavigationRoute SimplePlan(MapSnapshot map,MapMarker target) {
            NavigationRoute route=new NavigationRoute {Level=map.Live.level,Generation=map.Live.generation};
            route.Points.Add(new HeightPoint(target.position));return route;
        }
        private sealed class Simulation {
            internal NavigationExplorer Explorer;
            internal MapSnapshot Map;
            internal long Now=100000,Nonce=1000;
            internal Simulation(MapSnapshot map) : this(map,SimplePlan) { }
            internal Simulation(MapSnapshot map,Func<MapSnapshot,MapMarker,NavigationRoute> planner) {
                Map=map;Explorer=new NavigationExplorer(planner);Explorer.Start(Map,Now);
            }
            internal ExploreCommand Tick(long advance) {
                Now+=advance;Map.Live.timestamp_ms=Now;++Map.Live.update;
                ExploreCommand result=Explorer.Tick(Map,Now);
                if(result.Stop && Map.Live.navigation_ai!=null){Map.Live.navigation_ai.active=false;Map.Live.navigation_ai.state="stopped";}
                if(result.Route!=null){Explorer.Dispatched(++Nonce,Now);Map.Live.navigation_ai=new MapAi {nonce=Nonce,active=true,state="following"};}
                return result;
            }
            internal ExploreCommand Block(string reason) {
                ExploreCommand command=Explorer.RouteBlocked(reason,Now);
                if(command.Stop){Map.Live.navigation_ai.active=false;Map.Live.navigation_ai.state="stopped";}
                Tick(0);return command;
            }
            internal void Reach(MapMarker exit) { Map.Live.player.position=(float[])exit.position.Clone();Tick(200); }
            internal void Arrive(MapSnapshot next) { Map=next;Tick(200);Tick(700); }
        }
        private static ExploreExit Remember(MapMarker marker,uint? destination) {
            return new ExploreExit {key=NavigationExplorer.ExitKey(marker),position=marker.position,rawDestination=marker.destination_code,destination=destination};
        }
        private static ToolStripMenuItem FindItem(ToolStripItemCollection items,string label) {
            foreach(ToolStripItem item in items){var menu=item as ToolStripMenuItem;if(menu==null)continue;if(menu.Text==label)return menu;var child=FindItem(menu.DropDownItems,label);if(child!=null)return child;}return null;
        }
        internal static ToolStripMenuItem FindCommand(Control parent,string label) {
            var map=parent as NavigationMapWindow;if(map!=null&&map.SettingsSections!=null)foreach(var section in map.SettingsSections){var setting=FindItem(section.DropDownItems,label);if(setting!=null)return setting;}
            foreach(Control child in parent.Controls){var menu=child as MenuStrip;if(menu!=null){var item=FindItem(menu.Items,label);if(item!=null)return item;}var found=FindCommand(child,label);if(found!=null)return found;}return null;
        }
        private static void WriteSnapshot(string directory,MapSnapshot map) {
            map.Live.timestamp_ms=NavigationExplorer.Clock;++map.Live.update;
            WriteExclusive(Path.Combine(directory,"mesh.json"),delegate(Stream file){new DataContractJsonSerializer(typeof(MapGeometry)).WriteObject(file,map.Mesh);});
            WriteExclusive(Path.Combine(directory,"live.json"),delegate(Stream file){new DataContractJsonSerializer(typeof(MapLive)).WriteObject(file,map.Live);});
        }
        // RGO: the map window under test reads these files on its own refresh;
        // File.Create fails while its read handle is open, so wait for it (up
        // to 2 s) instead of failing the test on a timing collision.
        private static void WriteExclusive(string path,Action<Stream> write) {
            for(int attempt=0;;attempt++) {
                try {using(FileStream file=File.Create(path))write(file);return;}
                catch(IOException) {if(attempt>=80)throw;System.Threading.Thread.Sleep(25);}
            }
        }
        private static string[] Command(string directory) {
            using(var file=new FileStream(Path.Combine(directory,"ai-command.txt"),FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete))
            using(var reader=new StreamReader(file))return reader.ReadToEnd().Split(new char[]{' ','\n','\r'},StringSplitOptions.RemoveEmptyEntries);
        }
        internal static string[] WaitCommand(NavigationMapWindow window,string directory,bool moving,string oldNonce=null) {
            long end=NavigationExplorer.Clock+5000;
            while(NavigationExplorer.Clock<end) {
                Application.DoEvents();window.RefreshMap();
                if(File.Exists(Path.Combine(directory,"ai-command.txt"))) {
                    try {
                        string[] cmd=Command(directory);
                        if(cmd.Length>6&&(cmd[6]!="0")==moving&&(oldNonce==null||cmd[3]!=oldNonce))return cmd;
                    }catch(IOException){}
                }
                System.Threading.Thread.Sleep(25);
            }
            throw new Exception("Timed out waiting for shared navigation command; moving="+moving);
        }
        internal static int Run(string directory) {
            checks=0;
            MapMarker entrance=Exit(1,0,0),forward=Exit(2,500,0);
            Simulation s=new Simulation(Room(1,1,entrance,forward));
            Check(s.Tick(0).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(forward),"picked arrival doorway before new exit");
            string stable=NavigationExplorer.ExitKey(forward);forward.address=900;
            Check(NavigationExplorer.ExitKey(forward)==stable,"actor reallocation changed exit identity");
            MapMarker sameCode=Exit(3,600,0);sameCode.destination_code=forward.destination_code;
            Check(NavigationExplorer.ExitKey(sameCode)!=stable,"two exits sharing a destination collapsed");
            s.Reach(forward);s.Arrive(Room(2,2,Exit(20,0,0)));
            Check(s.Explorer.History.rooms[0].exits[1].destination==2,"did not learn observed destination");
            Check(!s.Explorer.History.rooms[1].exits[0].destination.HasValue,"invented reverse connection");
            Check(s.Explorer.History.rooms[0].exits[1].destination!=forward.destination_code,"treated raw destination as room ID");

            // A -> B -> C -> A has no unexplored exits left. It must not start again.
            MapMarker ab=Exit(10,500,0),bc=Exit(11,500,0),ca=Exit(12,500,0);
            s=new Simulation(Room(10,1,ab));s.Tick(0);s.Reach(ab);s.Arrive(Room(11,2,bc));
            Check(s.Tick(700).Route!=null,"did not continue in new room");s.Reach(bc);s.Arrive(Room(12,3,ca));
            s.Tick(700);s.Reach(ca);s.Arrive(Room(10,4,ab));
            Check(s.Tick(700).Stop && !s.Explorer.Running,"circular route did not terminate");
            Check(s.Tick(700).Route==null,"stopped loop restarted itself");
            Check(s.Explorer.History.rooms.Count==3,"room generations counted as new rooms");

            // Directed known backtracking: B -> A -> C, where C still has an unknown exit.
            MapMarker ba=Exit(21,500,0),ac=Exit(22,700,0),ab2=Exit(23,500,0),unknown=Exit(24,800,0);
            s=new Simulation(Room(21,1,ba));
            s.Explorer.History.rooms[0].exits[0].destination=20;
            s.Explorer.History.rooms.Add(new ExploreRoom {level=20,exits=new List<ExploreExit>{Remember(ab2,21),Remember(ac,22)}});
            s.Explorer.History.rooms.Add(new ExploreRoom {level=22,exits=new List<ExploreExit>{Remember(unknown,null)}});
            Check(s.Tick(0).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(ba),"failed deliberate backtracking to frontier");
            s.Reach(ba);s.Arrive(Room(20,2,ab2,ac));s.Tick(700);
            Check(s.Explorer.TargetKey==NavigationExplorer.ExitKey(ac),"backtracking bounced back into exhausted room");

            // Geometric failures are remembered, not retried every update.
            int plans=0;MapMarker bad=Exit(30,450,0),good=Exit(31,700,0);
            s=new Simulation(Room(30,1,bad,good),delegate(MapSnapshot map,MapMarker exit){++plans;if(exit==bad)throw new InvalidDataException("Disconnected floor");return SimplePlan(map,exit);});
            Check(s.Tick(0).Route==null && plans==1,"did not record disconnected route");
            Check(s.Tick(400).Route!=null && plans==2 && s.Explorer.TargetKey==NavigationExplorer.ExitKey(good),"did not choose alternative exit");
            Check(s.Explorer.Describe(30,bad).StartsWith("Route failed:"),"blocked reason not exposed");

            // Arrival at the marker is not transition success.
            s=new Simulation(Room(31,1,good));s.Tick(0);s.Reach(good);
            s.Map.Live.navigation_ai.active=false;s.Map.Live.navigation_ai.state="approach_complete";
            s.Tick(200);Check(!s.Explorer.MayHeartbeat,"kept moving after approach completion");
            Check(s.Tick(2600).Stop && !s.Explorer.History.rooms[0].exits[0].destination.HasValue,"approach falsely confirmed a destination");
            Check(s.Tick(600).Stop && !s.Explorer.Running,"blocked doorway retried indefinitely");

            // Progress permits a retry, but repeated toggles and reloading do not.
            s=new Simulation(Room(32,1,bad),delegate(MapSnapshot map,MapMarker exit){throw new InvalidDataException("Locked route");});
            s.Tick(0);s.Tick(400);Check(!s.Explorer.Running,"failed single route did not stop");
            int revision=s.Explorer.History.unlockRevision;
            s.Map.Live.progression.inventory.red_key=true;s.Explorer.Start(s.Map,s.Now);s.Tick(0);
            Check(s.Explorer.History.unlockRevision==revision+1 && s.Explorer.History.rooms[0].exits[0].attempts==2,"new key did not reconsider blocked exit");
            s.Explorer.Stop("test");s.Map.Live.progression.inventory.red_key=false;s.Explorer.Start(s.Map,s.Now);s.Tick(0);
            s.Explorer.Stop("test");s.Map.Live.progression.inventory.red_key=true;s.Explorer.Start(s.Map,s.Now);s.Tick(0);
            Check(s.Explorer.History.unlockRevision==revision+1 && s.Explorer.History.rooms[0].exits[0].attempts==2,"repeated inventory toggle created endless retries");

            s=new Simulation(Room(40,1,good));s.Tick(0);s.Map.Live.navigation_ai.active=false;s.Map.Live.navigation_ai.state="manual_takeover";
            Check(s.Tick(200).Stop && !s.Explorer.Running,"manual input did not stop explorer");
            Check(s.Tick(200).Route==null,"manual cancellation rearmed controller");
            s=new Simulation(Room(40,1,good));s.Tick(0);s.Now+=6000;
            Check(s.Explorer.Tick(s.Map,s.Now).Stop && !s.Explorer.Running,"stale map did not stop");
            s=new Simulation(Room(40,1,good));s.Tick(0);s.Map.Live.update=-5;
            Check(s.Tick(200).Stop && !s.Explorer.Running,"rewound game state accepted");
            s=new Simulation(Room(40,1,good));s.Tick(0);s.Map.Live.navigation_ai=new MapAi();
            Check(s.Tick(3200).Stop && !s.Explorer.Running,"missing native acknowledgement did not stop");
            s=new Simulation(Room(40,1,good));s.Tick(0);s.Map.Live.navigation_ai.active=false;s.Map.Live.navigation_ai.state="calibration_blocked";
            Check(s.Tick(200).Stop && s.Explorer.Describe(40,good).Contains("recovery"),"movement failure did not pause for local recovery");
            s=new Simulation(Room(40,1,good));s.Tick(0);s.Reach(good);
            s.Map.Live.clearing_active=false;s.Tick(200);s.Map.Live.clearing_active=true;
            Check(s.Tick(200).Stop && !s.Explorer.Running,"pause/resume restarted autonomous movement");

            // A decoded scripted camera can finish without abandoning the exit.
            // Unclassified pauses above still require an explicit restart.
            s=new Simulation(Room(40,1,good));s.Tick(0);
            s.Map.Live.clearing_active=false;s.Map.Live.scripted_camera=true;
            Check(s.Tick(200).Stop && s.Explorer.Running && !s.Explorer.MayHeartbeat,"scripted camera did not release movement and retain target");
            Check(s.Tick(1000).Route==null,"scripted camera rearmed movement");
            s.Map.Live.clearing_active=true;s.Map.Live.scripted_camera=false;
            Check(s.Tick(200).Stop && s.Explorer.Running,"scripted camera completion discarded route");
            s.Tick(200);Check(s.Tick(800).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(good),"scripted camera did not replan same exit after settling");
            s=new Simulation(Room(40,1,good));s.Tick(0);s.Map.Live.clearing_active=false;s.Map.Live.scripted_camera=true;s.Tick(200);
            ++s.Map.Live.navigation_ai.manual_inputs;
            Check(s.Tick(200).Stop && !s.Explorer.Running,"manual input during cutscene allowed automatic restart");
            s=new Simulation(Room(40,1,good));s.Tick(0);s.Map.Live.clearing_active=false;s.Map.Live.scripted_camera=true;s.Tick(200);
            Check(s.Tick(15100).Stop && !s.Explorer.Running,"scripted camera wait was unbounded");
            // Brief load gaps near a confirmed approach can be followed across rooms.
            s=new Simulation(Room(50,1,good));s.Tick(0);s.Reach(good);
            Check(s.Explorer.MissingMap(s.Now+100).Stop && s.Explorer.Running && !s.Explorer.MayHeartbeat,"load gap did not release movement");
            s.Arrive(Room(51,2,Exit(50,500,0)));
            Check(s.Explorer.Running && s.Explorer.History.rooms[0].exits[0].destination==51,"valid transition lost across load gap");
            s=new Simulation(Room(50,1,good));s.Tick(0);s.Reach(good);s.Explorer.MissingMap(s.Now+100);
            Check(s.Explorer.MissingMap(s.Now+20200).Stop && !s.Explorer.Running,"load gap waited indefinitely");
            s=new Simulation(Room(50,1,good));s.Tick(0);s.Tick(200);s.Arrive(Room(51,2,Exit(50,500,0)));
            Check(!s.Explorer.Running && !s.Explorer.History.rooms[0].exits[0].destination.HasValue,"unrelated load invented exit connection");
            s=new Simulation(Room(50,1,good));s.Tick(0);s.Reach(good);s.Arrive(Room(50,2,good));s.Tick(11000);s.Tick(700);
            Check(!s.Explorer.Running && s.Explorer.History.rooms.Count==1,"same-room exit loop continued");
            s=new Simulation(Room(50,1,good));Check(s.Tick(15*60*1000+1).Stop && !s.Explorer.Running,"exploration run limit missing");

            s=new Simulation(Room(55,1,good));s.Tick(0);s.Reach(good);
            s.Map.Live.generation=2;s.Map.Mesh.generation=2;s.Tick(200);
            Check(!s.Explorer.History.rooms[0].exits[0].destination.HasValue && !s.Explorer.MayHeartbeat,"source generation refresh falsely confirmed a room");
            s.Arrive(Room(56,2,bad));
            Check(s.Explorer.History.rooms[0].exits[0].destination==56,"lost destination after transient source snapshot");
            s=new Simulation(Room(57,1,good));s.Tick(0);s.Reach(good);
            s.Map.Live.generation=2;s.Map.Mesh.generation=2;s.Map.Live.transition_confirm=true;
            Check(s.Tick(200).Confirm,"area-clear confirmation not requested");
            Check(!s.Tick(200).Confirm,"area-clear confirmation repeated");
            s.Map.Live.transition_confirm=false;s.Arrive(Room(58,2,bad));
            Check(s.Explorer.History.rooms[0].exits[0].destination==58,"acknowledged area-clear transition not learned");
            s=new Simulation(Room(59,1,bad),delegate(MapSnapshot map,MapMarker exit){throw new InvalidDataException("Blocked");});
            s.Tick(0);s.Explorer.RetryRoom(s.Map,s.Now);s.Explorer.Start(s.Map,s.Now);s.Tick(0);
            Check(s.Explorer.History.rooms[0].exits[0].attempts==2,"explicit room retry erased attempts or did not permit another try");
            s.Explorer.Stop("test");s.Map.Live.transition_confirm=true;bool promptRejected=false;
            try{s.Explorer.Start(s.Map,s.Now);}catch(InvalidDataException){promptRejected=true;}
            Check(promptRejected,"exploration began inside an unrelated transition prompt");
            // Real surface planner extends through a doorway only over known floor.
            MapSnapshot floor=Room(60,1,good);NavigationRoute approach=NavigationRoute.Plan(floor,good.position);
            NavigationRoute crossing=NavigationRoute.PlanExit(floor,good);
            Check(crossing.Points.Count>approach.Points.Count && crossing.Points[crossing.Points.Count-1].X>good.position[0],"did not plan through mapped doorway");
            MapMarker boundary=Exit(60,990,0);crossing=NavigationRoute.PlanExit(floor,boundary);
            Check(crossing.Points[crossing.Points.Count-1].X==990,"crossing left known floor");


            // A real trigger's origin can be beside a sloping wall. Cross via
            // the body-clear centre lane, not a nearby point before its plane.
            MapMarker offCentre=Exit(601,-13,160);offCentre.position[1]=63;
            offCentre.normal=new float[]{0,0,-1};offCentre.radius=160;offCentre.plane_d=160;offCentre.directional=-1;
            var laneMap=Room(601,1,offCentre);laneMap.Live.player.position=new float[]{0,0,-120};
            laneMap.Mesh.vertices=new float[][] {
                new float[]{-41,0,-200},new float[]{41,0,-200},new float[]{-41,0,300},new float[]{41,0,300},
                new float[]{-24,125,-200},new float[]{-24,125,300},
                new float[]{41,125,-200},new float[]{41,125,300}};
            laneMap.Mesh.triangles=new MapFace[]{
                new MapFace{v=new[]{0,2,1},normal=new float[]{0,1,0}},new MapFace{v=new[]{1,2,3},normal=new float[]{0,1,0}},
                new MapFace{v=new[]{0,4,2},normal=new float[]{1,0,0}},new MapFace{v=new[]{2,4,5},normal=new float[]{1,0,0}},
                new MapFace{v=new[]{1,3,6},normal=new float[]{-1,0,0}},new MapFace{v=new[]{3,7,6},normal=new float[]{-1,0,0}}};
            var laneRoute=NavigationRoute.PlanExit(laneMap,offCentre);
            var laneEnd=laneRoute.Points[laneRoute.Points.Count-1];
            Check(laneEnd.Z>offCentre.position[2]+8&&Math.Abs(laneEnd.X-offCentre.position[0])>=8,"exit route stopped before plane or targeted wall");
            Check(NavigationRoute.ClearWalk(laneMap,new MapLayers(laneMap.Mesh).Floors,laneRoute.Points[laneRoute.Points.Count-2],laneEnd),"exit lane lacks body clearance");
            laneMap.Live.progression.nodes=new[]{new MapInteraction{address=offCentre.address,condition_known=true,condition_met=false}};
            bool inactive=false;try{NavigationRoute.PlanExit(laneMap,offCentre);}catch(InvalidDataException){inactive=true;}
            Check(inactive,"inactive exit variant accepted");
            laneMap.Live.progression.nodes[0].condition_met=true;
            laneMap.Live.progression.nodes[0].access_known=true;
            laneMap.Live.progression.nodes[0].access_allowed=false;
            laneMap.Live.progression.nodes[0].status="key_missing";
            laneMap.Live.progression.nodes[0].requirement="Yellow key";
            bool locked=false;try{NavigationRoute.PlanExit(laneMap,offCentre);}catch(InvalidDataException e){locked=e.Message.Contains("Yellow key");}
            Check(locked,"missing yellow key treated as clear passage");
            var stableAccess=new NavigationExplorer();var accessMap=Room(602,1,offCentre);
            accessMap.Live.progression.nodes=new[]{new MapInteraction{kind="gate",door_id=2,
                status="enemy_lock_cleared",position=new float[]{0,0,0}}};
            stableAccess.ObserveIdle(accessMap,accessMap.Live.timestamp_ms);
            var accessRevision=stableAccess.History.unlockRevision;
            accessMap.Live.progression.nodes[0].position[1]=120;
            stableAccess.ObserveIdle(accessMap,accessMap.Live.timestamp_ms);
            Check(stableAccess.History.unlockRevision==accessRevision,"door animation manufactured new unlock progress");

            // A small elevated trigger can require a shallower crossing before
            // mapped floor ends; a fixed 32-unit extension rejects this ramp.
            laneMap.Live.progression.nodes=new MapInteraction[0];
            offCentre.position[1]=48;offCentre.radius=64;
            foreach(int i in new[]{2,3,5,7})laneMap.Mesh.vertices[i][2]=180;
            var shallow=NavigationRoute.PlanExit(laneMap,offCentre);
            var shallowEnd=shallow.Points[shallow.Points.Count-1];
            Check(shallowEnd.Z>168&&shallowEnd.Z<=180,"small ramp trigger did not use supported shallow crossing");
            // A low object outside a narrow door leaves body clearance but not
            // the optional comfort envelope. Only the final doorway leg narrows.
            laneMap.Live.actors=new[]{new MapActor{address=990,position=new float[]{-42,0,65}}};
            var throatBox=new MapCollisionModel{address=990,enabled=true,lower=new float[]{-60,0,40},upper=new float[]{-24,13,90}};
            laneMap.Live.collision.models=new[]{throatBox};
            var throatRoute=NavigationRoute.PlanExit(laneMap,offCentre);
            Check(throatRoute.Points[throatRoute.Points.Count-2].Z<40&&
                NavigationRoute.ClearWalk(laneMap,new MapLayers(laneMap.Mesh).Floors,
                    throatRoute.Points[throatRoute.Points.Count-2],throatRoute.Points[throatRoute.Points.Count-1]),
                "door throat did not preserve full body clearance");
            throatBox.upper[0]=10;bool solidRefused=false;
            try{NavigationRoute.PlanExit(laneMap,offCentre);}catch(InvalidDataException){solidRefused=true;}
            Check(solidRefused,"door throat fallback ignored a solid body obstruction");


            s=new Simulation(Room(61,1,good));s.Tick(0);s.Reach(good);
            MapSnapshot interrupted=Room(62,2,bad);interrupted.Live.navigation_ai.manual_inputs=1;s.Arrive(interrupted);
            Check(!s.Explorer.Running && !s.Explorer.History.rooms[0].exits[0].destination.HasValue,"manual input during room load was lost");
            s=new Simulation(Room(61,1,good));s.Map.Live.navigation_ai.manual_inputs=1;
            Check(s.Tick(0).Stop && !s.Explorer.Running,"manual input before dispatch was lost");
            MapSnapshot smooth=Room(63,1,good);
            System.Collections.Generic.List<HeightSurface> surfaces=new MapLayers(smooth.Mesh).Floors;
            Check(NavigationRoute.StraightWalk(smooth.Mesh,surfaces,new HeightPoint(0,0,0),new HeightPoint(400,0,0)),"open straight segment refused");
            Check(!NavigationRoute.StraightWalk(smooth.Mesh,surfaces,new HeightPoint(0,0,0),new HeightPoint(400,100,0)),"shortcut crossed floor heights");
            smooth.Mesh.vertices=new float[][] {new float[]{-1000,0,-1000},new float[]{1000,0,-1000},new float[]{-1000,0,1000},new float[]{1000,0,1000},new float[]{200,0,-200},new float[]{200,200,-200},new float[]{200,0,200},new float[]{200,200,200}};
            smooth.Mesh.triangles=new MapFace[]{smooth.Mesh.triangles[0],smooth.Mesh.triangles[1],new MapFace {v=new int[]{4,5,6}},new MapFace {v=new int[]{5,7,6}}};
            Check(!NavigationRoute.StraightWalk(smooth.Mesh,surfaces,new HeightPoint(0,0,0),new HeightPoint(400,0,0)),"shortcut crossed a wall");
            MapGeometry gap=new MapGeometry {vertices=new float[][]{new float[]{-100,0,-100},new float[]{199,0,-100},new float[]{-100,0,100},new float[]{199,0,100},new float[]{201,0,-100},new float[]{500,0,-100},new float[]{201,0,100},new float[]{500,0,100}},triangles=new MapFace[]{new MapFace {v=new int[]{0,2,1},normal=new float[]{0,1,0}},new MapFace {v=new int[]{1,2,3},normal=new float[]{0,1,0}},new MapFace {v=new int[]{4,6,5},normal=new float[]{0,1,0}},new MapFace {v=new int[]{5,6,7},normal=new float[]{0,1,0}}}};
            Check(!NavigationRoute.StraightWalk(gap,new MapLayers(gap).Floors,new HeightPoint(0,0,0),new HeightPoint(400,0,0)),"shortcut bridged a thin gap");
            MapSnapshot steep=Room(64,1,good);
            steep.Mesh.vertices=new float[][] {new float[]{0,0,0},new float[]{200,400,0},new float[]{0,0,200},new float[]{200,400,200}};
            steep.Live.player.position=new float[]{20,40,20};bool slopeRejected=false;
            try {NavigationRoute.Plan(steep,new float[]{180,360,180});}catch(InvalidDataException){slopeRejected=true;}
            Check(slopeRejected,"steep rock face treated as ordinary walking floor");
            // History survives closing the map, without remembering an armed command.
            s=new Simulation(Room(70,1,good));s.Tick(0);s.Reach(good);s.Arrive(Room(71,2,Exit(70,500,0)));
            string path=Path.Combine(directory,"explorer-history-test");Directory.CreateDirectory(path);s.Explorer.Save(path);
            NavigationExplorer loaded=NavigationExplorer.Load(path);
            Check(!loaded.Running && loaded.History.rooms.Count==2 && loaded.History.rooms[0].exits[0].destination==71,"history persistence lost connection or armed movement");
            File.WriteAllText(Path.Combine(path,"exploration-history.json"),"{broken");
            bool rejected=false;try{NavigationExplorer.Load(path);}catch(InvalidDataException){rejected=true;}
            Check(rejected,"corrupt history accepted");
            // A disappearing exit cannot keep a phantom frontier alive.
            s=new Simulation(Room(80,1,bad,good));s.Map.Live.exits=new MapMarker[]{good};s.Tick(0);
            Check(s.Explorer.History.rooms[0].exits[0].absent,"absent exit remained a frontier");
            s.Explorer.Stop("test");s.Map.Live.exits=new MapMarker[]{bad,good};s.Explorer.Start(s.Map,s.Now);
            Check(!s.Explorer.History.rooms[0].exits[0].absent,"reappearing exit stayed unavailable");
            s=new Simulation(Room(80,1,good));s.Tick(0);s.Reach(good);
            MapSnapshot arriving=Room(81,2,bad);arriving.Live.clearing_active=false;s.Arrive(arriving);
            Check(s.Explorer.Running && !s.Explorer.MayHeartbeat,"did not suspend during destination load");
            s.Tick(700);s.Map.Live.clearing_active=true;s.Tick(700);
            Check(s.Tick(700).Route!=null,"did not continue after confirmed destination finished loading");

            // Exercise actual buttons, command files, and serialized native acknowledgements.
            string ui=Path.Combine(directory,"explorer-ui-test");Directory.CreateDirectory(ui);
            MapSnapshot uiMap=Room(90,1,good);WriteSnapshot(ui,uiMap);
            using(NavigationMapWindow window=new NavigationMapWindow(ui)) {
                window.StartPosition=FormStartPosition.Manual;window.Location=new Point(-32000,-32000);window.ShowInTaskbar=false;window.Show();Application.DoEvents();
                ToolStripMenuItem explore=FindCommand(window,"Explore automatically"),stop=FindCommand(window,"Stop AI");
                Check(explore!=null && stop!=null,"explorer controls missing");
                Check(FindCommand(window,"Retry room exits")!=null,"explicit room retry control missing");
                explore.PerformClick();string[] command=WaitCommand(window,ui,true);
                Check(command[0]=="JFGNAV3" && command[1]=="90" && Int32.Parse(command[6])>0,"Explore button did not dispatch route");
                uiMap.Live.navigation_ai=new MapAi {nonce=Int64.Parse(command[3]),active=false,state="manual_takeover"};WriteSnapshot(ui,uiMap);window.RefreshMap();
                command=WaitCommand(window,ui,false);Check(command[6]=="0","native manual takeover did not send stop");
                string stopped=File.ReadAllText(Path.Combine(ui,"ai-command.txt"));window.RefreshMap();
                Check(File.ReadAllText(Path.Combine(ui,"ai-command.txt"))==stopped,"UI rearmed after manual takeover");
                System.Threading.Thread.Sleep(150);window.RefreshMap();explore.PerformClick();Check(WaitCommand(window,ui,true)[6]!="0","explicit restart failed");
                stop.PerformClick();Check(WaitCommand(window,ui,false)[6]=="0","Stop AI did not cancel exploration");
                System.Threading.Thread.Sleep(150);window.RefreshMap();explore.PerformClick();WaitCommand(window,ui,true);window.Close();System.Threading.Thread.Sleep(150);Check(Command(ui)[6]=="0","closing map did not release controls");
            }

            // A blocked forward path can return through the arrival doorway.
            int entrancePlans=0;
            s=new Simulation(Room(100,1,entrance,forward),delegate(MapSnapshot map,MapMarker exit){
                if(exit==entrance){++entrancePlans;return SimplePlan(map,exit);}
                throw new InvalidDataException("Forward gate blocked");
            });
            s.Tick(0);Check(s.Tick(400).Route!=null && entrancePlans==1,"blocked forward exit prevented a valid return");
            Check(s.Explorer.Describe(100,entrance).Contains("Return route"),"return candidate not explained");
            // One-door item room returns to its parent; observed edges prevent
            // bouncing back into the now exhausted room.
            var toItem=Exit(200,500,0);var back=Exit(201,0,0);var onward=Exit(202,700,0);
            s=new Simulation(Room(200,1,toItem,onward));s.Tick(0);s.Reach(toItem);
            s.Arrive(Room(201,2,back));
            Check(s.Tick(700).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(back),"one-door room could not return");
            s.Reach(back);s.Arrive(Room(200,3,toItem,onward));
            Check(s.Tick(700).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(onward),"returned into cleared item room");
            bool unlocked=false;
            s=new Simulation(Room(203,1,back),delegate(MapSnapshot map,MapMarker exit) {
                if(!unlocked)throw new InvalidDataException("Return door needs key");
                return SimplePlan(map,exit);
            });
            s.Tick(0);Check(s.Tick(400).Stop,"locked return doorway did not stop");
            unlocked=true;s.Map.Live.progression.inventory.red_key=true;s.Explorer.Start(s.Map,s.Now);
            Check(s.Tick(0).Route!=null,"new key could not reopen a previously blocked return door");
            // Starting after manual movement must retain the observed entrance.
            MapSnapshot idle=Room(101,1,entrance,forward);
            NavigationExplorer idleExplorer=new NavigationExplorer(SimplePlan);idleExplorer.ObserveIdle(idle,100000);
            idle.Live.player.position=new float[]{500,0,0};idleExplorer.Start(idle,100000);
            Check(idleExplorer.Tick(idle,100000).Route!=null && idleExplorer.TargetKey==NavigationExplorer.ExitKey(forward),"start reclassified nearest exit after manual walking");
            idleExplorer.Stop("test");string arrivalFolder=Path.Combine(directory,"arrival-history");Directory.CreateDirectory(arrivalFolder);idleExplorer.Save(arrivalFolder);
            NavigationExplorer resumed=NavigationExplorer.Load(arrivalFolder);resumed.Start(idle,100000);
            Check(resumed.Tick(idle,100000).Route!=null && resumed.TargetKey==NavigationExplorer.ExitKey(forward),"reopening map lost arrival doorway");
            // A live obstacle pauses movement and replans toward the same target first.
            s=new Simulation(Room(102,1,bad,good));s.Tick(0);
            Check(s.Block("Moving door blocked route").Stop && s.Explorer.Running,"dynamic collision stopped all exploration");
            Check(!s.Explorer.MayHeartbeat && s.Explorer.TargetKey==NavigationExplorer.ExitKey(bad),"recovery lost target or kept movement armed");
            Check(s.Tick(500).Route==null,"replanned before movement settled");
            Check(s.Tick(250).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(bad),"dynamic collision abandoned recoverable exit");
            Check(!s.Explorer.Describe(102,bad).StartsWith("Route failed:"),"transient collision permanently blocked exit");
            for(int i=0;i<3;i++){s.Block("Moving door blocked route");Check(s.Tick(750).Route!=null,"bounded recovery stopped too soon");}
            Check(s.Block("Moving door blocked route").Stop,"recovery exhaustion failed to release movement");
            Check(s.Explorer.Describe(102,bad).Contains("Local recovery limit"),"persistent failure did not become blocked");
            Check(s.Tick(750).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(good),"recovery limit did not select another exit");
            // No progress watchdog releases a controller that remains active at a wall.
            s=new Simulation(Room(103,1,bad,good));s.Tick(0);s.Tick(200);
            Check(s.Tick(1900).Stop && !s.Explorer.MayHeartbeat,"wall stall kept receiving movement heartbeats");s.Tick(0);
            Check(s.Tick(750).Route!=null && s.Explorer.TargetKey==NavigationExplorer.ExitKey(bad),"wall stall did not retry same destination");
            s=new Simulation(Room(104,1,good));s.Tick(0);s.Tick(200);
            for(int i=0;i<4;i++){s.Map.Live.player.position[0]+=10;Check(!s.Tick(1000).Stop,"steady progress treated as wall stall");}
            // Refresh a real list while its selection is intentionally above the viewport.
            string scroll=Path.Combine(directory,"scroll-ui-test");Directory.CreateDirectory(scroll);
            MapSnapshot rows=Room(105,1,good);rows.Live.progression.nodes=new MapInteraction[60];
            for(int i=0;i<60;i++)rows.Live.progression.nodes[i]=new MapInteraction {address=(uint)(1000+i),kind="exit",label="Exit "+i,position=new float[]{i,0,0},action="enter_exit",status="unknown",requirement="Unknown",reward="Unknown",traversal="unknown"};
            WriteSnapshot(scroll,rows);
            using(NavigationMapWindow window=new NavigationMapWindow(scroll)) {
                window.ShowInTaskbar=false;window.StartPosition=FormStartPosition.Manual;window.Location=new Point(-32000,-32000);window.Show();Application.DoEvents();
                ListBox list=(ListBox)typeof(NavigationMapWindow).GetField("interactionList",System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance).GetValue(window);
                list.SelectedIndex=0;list.TopIndex=30;int top=list.TopIndex;
                for(int i=0;i<15;i++) {rows.Live.progression.nodes[40].status="update "+i;WriteSnapshot(scroll,rows);window.RefreshMap();}
                Check(list.TopIndex==top && ((MapInteraction)list.SelectedItem).address==1000,"live refresh scrolled selected row into view");
                list.SelectedIndex=35;list.TopIndex=30;window.RefreshMap();
                Check(((MapInteraction)list.SelectedItem).address==1035 && list.TopIndex==30,"exit selection lost on refresh");
                List<MapInteraction> changedRows=new List<MapInteraction>(rows.Live.progression.nodes);changedRows.RemoveAt(5);rows.Live.progression.nodes=changedRows.ToArray();WriteSnapshot(scroll,rows);window.RefreshMap();
                Check(((MapInteraction)list.Items[list.TopIndex]).address==1030 && ((MapInteraction)list.SelectedItem).address==1035,"row removal lost scroll anchor or selection");
                rows.Live.generation=2;rows.Mesh.generation=2;WriteSnapshot(scroll,rows);window.RefreshMap();
                Check(list.TopIndex==0 && list.SelectedIndex==0,"new room retained unrelated selection");
                var filter=(ComboBox)typeof(NavigationMapWindow).GetField("interactionFilter",System.Reflection.BindingFlags.NonPublic|System.Reflection.BindingFlags.Instance).GetValue(window);
                filter.SelectedIndex=1;Check(list.Items.Count==0,"chest filter displayed exit rows");
                filter.SelectedIndex=3;Check(list.Items.Count==59,"exit filter omitted loaded exits");
                filter.SelectedIndex=0;window.RefreshMap();Check(list.Items.Count==59,"returning to all interactions lost rows");
                window.Close();
            }
            // Exercise automatic obstruction recovery through the actual map window.
            string recovery=Path.Combine(directory,"explorer-obstacle-ui");Directory.CreateDirectory(recovery);
            MapSnapshot recoveryMap=Room(106,1,Exit(106,500,0),Exit(107,0,700));WriteSnapshot(recovery,recoveryMap);
            using(NavigationMapWindow window=new NavigationMapWindow(recovery)) {
                window.ShowInTaskbar=false;window.StartPosition=FormStartPosition.Manual;window.Location=new Point(-32000,-32000);window.Show();Application.DoEvents();
                FindCommand(window,"Explore automatically").PerformClick();string[] initial=WaitCommand(window,recovery,true);
                recoveryMap.Live.navigation_ai=new MapAi {nonce=Int64.Parse(initial[3]),active=true,state="following",waypoint=0};
                recoveryMap.Live.actors=new MapActor[]{new MapActor {address=12345,name="Moving door",position=new float[]{150,0,0}}};
                recoveryMap.Live.collision.models=new MapCollisionModel[]{new MapCollisionModel {address=12345,enabled=true,lower=new float[]{100,-5,-100},upper=new float[]{200,100,100}}};
                WriteSnapshot(recovery,recoveryMap);window.RefreshMap();
                Check(WaitCommand(window,recovery,false)[6]=="0","automatic route did not release input for new blocker");
                recoveryMap.Live.navigation_ai.active=false;recoveryMap.Live.navigation_ai.state="stopped";
                WriteSnapshot(recovery,recoveryMap);window.RefreshMap();
                System.Threading.Thread.Sleep(750);WriteSnapshot(recovery,recoveryMap);window.RefreshMap();string[] alternative=WaitCommand(window,recovery,true,initial[3]);
                Check(alternative[6]!="0" && alternative[3]!=initial[3],"map window failed to dispatch alternative after blockage");
                NavigationExplorer history=NavigationExplorer.Load(recovery);
                Check(history.History.rooms[0].exits[0].blocked=="" && history.History.rooms[0].exits[0].attempts==1,"recoverable map collision was persisted as unreachable exit");
                Check(File.Exists(Path.Combine(recovery,"last-route-stop.json")) && File.ReadAllText(Path.Combine(recovery,"route-events.tsv")).Contains("Moving door"),"exact failed route state was not retained");window.Close();
            }
            // A dropped door is an approach-and-observe target, not a permanent wall.
            s=new Simulation(DoorRoom(),NavigationRoute.PlanExit);ExploreCommand door=s.Tick(0);
            Check(door.Route!=null && door.Route.ApproachOnly,"closed proximity door made whole exit unreachable");
            Check(door.Route.Points[door.Route.Points.Count-1].X<400,"door approach crossed closed collision box");
            HeightPoint waiting=door.Route.Points[door.Route.Points.Count-1];s.Map.Live.player.position=new float[]{waiting.X,waiting.Y,waiting.Z};
            s.Map.Live.navigation_ai.active=false;s.Map.Live.navigation_ai.state="approach_complete";s.Map.Live.navigation_ai.waypoint=door.Route.Points.Count;
            Check(s.Tick(200).Route==null && !s.Explorer.MayHeartbeat,"kept pushing while waiting for door");
            s.Map.Live.collision.models[0].lower[1]=40;s.Map.Live.collision.models[0].upper[1]=190;
            Check(s.Tick(200).Route==null,"entered partially raised door without headroom");
            s.Map.Live.collision.models[0].lower[1]=100;s.Map.Live.collision.models[0].upper[1]=250;
            ExploreCommand opened=s.Tick(200);
            Check(opened.Route!=null && !opened.Route.ApproachOnly,"raised collision did not resume exit route");
            Check(!s.Explorer.History.rooms[0].exits[0].destination.HasValue,"door lift falsely counted as room transition");
            s=new Simulation(DoorRoom(),NavigationRoute.PlanExit);door=s.Tick(0);waiting=door.Route.Points[door.Route.Points.Count-1];
            s.Map.Live.player.position=new float[]{waiting.X,waiting.Y,waiting.Z};s.Map.Live.navigation_ai.active=false;s.Map.Live.navigation_ai.state="approach_complete";
            s.Tick(200);Check(s.Tick(8100).Stop && s.Explorer.Describe(107,s.Map.Live.exits[0]).Contains("door"),"locked door caused endless waiting or pushing");
            // King's hut: the trigger is BEFORE the closed curtain. Approach
            // from this room and wait on the curtain, not the earlier trigger.
            var hutExit=Exit(480,0,364);hutExit.position[1]=48;
            hutExit.normal=new float[]{0,0,-1};hutExit.plane_d=364;hutExit.radius=71;hutExit.directional=-1;
            var hut=Room(48,1,hutExit);hut.Live.player.position=new float[]{0,0,291};
            hut.Live.actors=new[]{new MapActor{address=4800,position=new float[]{0,9,390}}};
            var curtain=new MapCollisionModel{address=4800,enabled=true,lower=new float[]{-80,9,389},upper=new float[]{80,125,392}};
            var curtainNode=new MapInteraction{address=4800,kind="gate",action="pass_door",position=new float[]{0,9,390},
                status="opens_on_approach",access_known=true,access_allowed=true,approach_radius=90};
            hut.Live.collision.models=new[]{curtain};hut.Live.progression.nodes=new[]{curtainNode};
            var hutApproach=NavigationRoute.PlanExit(hut,hutExit);
            var hutWait=hutApproach.Points[hutApproach.Points.Count-1];
            Check(hutApproach.ApproachOnly&&hutWait.Z<369&&hutWait.Z>312,
                "trigger in front of curtain sent approach to its far side or outside opening radius");
            Check(hutApproach.CheckRemaining(hut,0)==null,"curtain approach crossed closed collision");
            Check(!hutApproach.GateCleared(hut),"trigger before curtain falsely proved passage was clear");
            curtain.lower[1]=40;curtain.upper[1]=156;
            Check(!hutApproach.GateCleared(hut),"partially raised curtain admitted player without headroom");
            curtain.lower[1]=100;curtain.upper[1]=216;
            Check(hutApproach.GateCleared(hut),"raised curtain did not clear the stored doorway");
            hut.Live.player.position=new[]{hutWait.X,hutWait.Y,hutWait.Z};
            var hutCross=NavigationRoute.PlanExit(hut,hutExit);
            var hutEnd=hutCross.Points[hutCross.Points.Count-1];
            Check(!hutCross.ApproachOnly&&hutEnd.Z>372&&hutEnd.Z<410,"opened curtain did not route through live exit sphere");
            curtain.lower[1]=9;curtain.upper[1]=125;
            hut.Live.player.position=new float[]{0,0,480};
            hutExit.position[2]=416;hutExit.normal[2]=1;hutExit.plane_d=-416;
            var reverseCurtain=NavigationRoute.PlanExit(hut,hutExit);
            Check(reverseCurtain.ApproachOnly&&reverseCurtain.Points[reverseCurtain.Points.Count-1].Z>412,
                "opposite side of curtain used the wrong approach face");
            // A sideways opening keeps the probe anchored to the original
            // doorway instead of following the moving door model.
            curtain.lower[0]+=200;curtain.upper[0]+=200;
            Check(reverseCurtain.GateCleared(hut),"clearance probe followed a sliding door away from its opening");
            curtain.lower[0]-=200;curtain.upper[0]-=200;
            curtainNode.access_allowed=false;curtainNode.status="key_missing";curtainNode.requirement="Yellow key";
            bool hutLocked=false;try{NavigationRoute.PlanExit(hut,hutExit);}catch(InvalidDataException e){hutLocked=e.Message.Contains("Yellow key");}
            Check(hutLocked,"curtain approach bypassed a real key requirement");
            s=new Simulation(Room(109,1,good));s.Tick(0);s.Block("terrain");s.Map.Live.navigation_ai.manual_inputs++;
            Check(s.Tick(750).Stop && !s.Explorer.Running,"manual input during recovery was ignored");
            s=new Simulation(Room(110,1,good));s.Tick(0);s.Block("terrain");s.Explorer.Stop("user stopped");
            Check(s.Tick(750).Route==null,"stopped recovery rearmed movement");
            s=new Simulation(Room(111,1,good));s.Tick(0);s.Block("terrain");
            s.Map.Live.player.position[0]+=20;Check(s.Tick(750).Route==null,"replanned while player was still coasting");
            s.Map.Live.player.position[0]+=20;Check(s.Tick(300).Route==null,"coasting did not restart settle timer");
            Check(s.Tick(500).Route!=null,"stable position did not allow recovery");
            return checks;
        }
    }
}
