using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.IO.Compression;
using System.Text;
namespace JfgLauncher {
    // Format knowledge and semantic identifiers only. All pixels and models are
    // read from the owner's verified ROM, never embedded or downloaded.
    internal static class InventoryImages {
        internal const string Version="inventory-v5";
        internal static string CacheDirectory { get { return Path.Combine(FirstRun.Root,"asset-cache",LocalSetup.RomSha1,Version); } }
        private static byte[] Read(Stream stream,long offset,int count) {
            if(offset<0||count<0||count>1048576||offset>stream.Length-count)throw new InvalidDataException("Invalid inventory asset range.");
            stream.Position=offset;byte[] data=new byte[count];int used=0,n;
            while(used<count&&(n=stream.Read(data,used,count-used))>0)used+=n;
            if(used!=count)throw new InvalidDataException("Truncated inventory asset.");return data;
        }
        private static uint U32(byte[] data,int p) {return (uint)(data[p]<<24|data[p+1]<<16|data[p+2]<<8|data[p+3]);}
        private static int U16(byte[] data,int p) {return data[p]<<8|data[p+1];}
        private static long[] Section(Stream rom,int id) {
            byte[] entry=Read(rom,0xb1754+id*4,8);long a=0xb1880L+U32(entry,0),b=0xb1880L+U32(entry,4);
            if(a<0xb1880||b<a||b>rom.Length)throw new InvalidDataException("Invalid ROM asset section.");return new[]{a,b};
        }
        private static byte[] Entry(Stream rom,int table,int data,int id) {
            long[] t=Section(rom,table),d=Section(rom,data);
            if(id<0||id>(t[1]-t[0]-8)/4)throw new InvalidDataException("Invalid inventory asset index.");
            byte[] offsets=Read(rom,t[0]+id*4,8);long a=U32(offsets,0),b=U32(offsets,4);
            if(b<a||b>d[1]-d[0]||b-a>1048576)throw new InvalidDataException("Invalid inventory asset length.");
            return Read(rom,d[0]+a,(int)(b-a));
        }
        internal static byte[] Inflate(byte[] data,int start) {
            if(start<0||start>data.Length-6)throw new InvalidDataException("Truncated compressed asset.");
            uint size=(uint)(data[start]|data[start+1]<<8|data[start+2]<<16|data[start+3]<<24);
            if(size<32||size>1048576)throw new InvalidDataException("Invalid expanded asset size.");
            var expanded=new byte[(int)size];int used=0,n;
            using(var input=new MemoryStream(data,start+5,data.Length-start-5,false))using(var inflate=new DeflateStream(input,CompressionMode.Decompress)) {
                while(used<expanded.Length&&(n=inflate.Read(expanded,used,expanded.Length-used))>0)used+=n;
                if(used!=expanded.Length||inflate.ReadByte()!=-1)throw new InvalidDataException("Invalid expanded asset length.");
            }
            return expanded;
        }
        internal static Bitmap Decode(byte[] encoded) {
            if(encoded==null||encoded.Length<32)throw new InvalidDataException("Truncated texture header.");
            byte[] raw=encoded[25]!=0?Inflate(encoded,32):encoded;
            int width=raw[0],height=raw[1],format=raw[2]&15;
            if(width==0||height==0||width>128||height>128||format>6)throw new InvalidDataException("Unsupported inventory texture format.");
            int bits=format==0?32:format==1||format==4?16:format==2||format==5?8:4;
            int stride=(width*bits+7)/8;
            if(stride%(format==0?16:8)!=0||raw.Length<32+stride*height)throw new InvalidDataException("Truncated inventory pixels.");
            var image=new Bitmap(width,height,PixelFormat.Format32bppArgb);
            try {
                for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
                    int p=(y*stride+x*bits/8)^((y&1)!=0?(format==0?8:4):0);p+=32;
                    int red,green,blue,alpha=255,v=raw[p];
                    if(format==0){red=v;green=raw[p+1];blue=raw[p+2];alpha=raw[p+3];}
                    else if(format==1){v=U16(raw,p);red=((v>>11)&31)*255/31;green=((v>>6)&31)*255/31;blue=((v>>1)&31)*255/31;alpha=(v&1)*255;}
                    else if(format==4){red=green=blue=v;alpha=raw[p+1];}
                    else if(format==5){red=green=blue=(v>>4)*17;alpha=(v&15)*17;}
                    else if(format==2){red=green=blue=v;}
                    else {v=(v>>((x&1)==0?4:0))&15;if(format==3)red=green=blue=v*17;else {red=green=blue=(v>>1)*255/7;alpha=(v&1)*255;}}
                    image.SetPixel(x,y,Color.FromArgb(alpha,red,green,blue));
                }
                return image;
            }catch{image.Dispose();throw;}
        }
        private static Bitmap Texture(Stream rom,int id){return Decode(Entry(rom,(id&0x8000)!=0?1:3,(id&0x8000)!=0?0:2,id&0x7fff));}
        private static Bitmap Cached(string path) {
            try {
                if(File.Exists(path)&&new FileInfo(path).Length<262144)using(var stream=File.OpenRead(path)) {
                    byte[] h=Read(stream,0,24);
                    if(U32(h,0)==0x89504e47&&U32(h,4)==0x0d0a1a0a&&U32(h,12)==0x49484452&&U32(h,16)>0&&U32(h,16)<=128&&U32(h,20)>0&&U32(h,20)<=128) {
                        stream.Position=0;using(var image=new Bitmap(stream))return new Bitmap(image);
                    }
                }
            }catch(InvalidDataException){}catch(IOException){}catch(UnauthorizedAccessException){}catch(ArgumentException){}catch(System.Runtime.InteropServices.ExternalException){}catch(OutOfMemoryException){}
            return null;
        }
        private static void WriteCache(string path,Action<string> write) {
            string temporary=path+"."+Guid.NewGuid().ToString("N")+".tmp";
            try{write(temporary);if(File.Exists(path))File.Replace(temporary,path,null);else File.Move(temporary,path);}
            catch(IOException){}catch(UnauthorizedAccessException){}catch(System.Runtime.InteropServices.ExternalException){}
            finally{try{if(File.Exists(temporary))File.Delete(temporary);}catch(IOException){}catch(UnauthorizedAccessException){}}
        }
        // Export only on the user's computer after verification. The native home
        // scene uses real vertices/UVs and textures, not shipped game thumbnails.
        internal static void ExportShips(string romPath,string destination) {
            using(var rom=LocalSetup.OpenVerifiedRom(romPath)) {
                WriteCache(destination,delegate(string temporary) {
                    using(var file=File.Create(temporary))using(var writer=new BinaryWriter(file)) {
                        writer.Write(0x3253474a); // JGS2: joint hierarchy and per-vertex limb, little endian
                        byte[] source=Encoding.UTF8.GetBytes(romPath);writer.Write(source.Length);writer.Write(source);
                        int[] models={356,357,358};writer.Write(models.Length);
                        foreach(int model in models) {
                            byte[] data=Inflate(Entry(rom,38,39,model),0);
                            int nv=U16(data,18),nt=U16(data,20),nb=U16(data,22),materials=data[16];
                            int tp=(int)U32(data,24),vp=(int)U32(data,28),fp=(int)U32(data,32),bp=(int)U32(data,36);
                            if(nv<3||nv>4096||nt<1||nt>8192||nb<1||nb>1024||materials>64||vp<0||vp+nv*10>data.Length||fp<0||fp+nt*16>data.Length||bp<0||bp+(nb+1)*16>data.Length||tp<0||tp+materials*8>data.Length)throw new InvalidDataException("Invalid background model.");
                            writer.Write(materials);
                            for(int m=0;m<materials;m++)using(var texture=Texture(rom,U16(data,tp+m*8+6))) {
                                writer.Write(texture.Width);writer.Write(texture.Height);
                                for(int y=0;y<texture.Height;y++)for(int x=0;x<texture.Width;x++) {
                                    var c=texture.GetPixel(x,y);writer.Write((byte)(c.R*c.A/255));writer.Write((byte)(c.G*c.A/255));writer.Write((byte)(c.B*c.A/255));writer.Write(c.A);
                                }
                            }
                            // Model +0x54 holds 16-byte joints: parent, animation indices,
                            // and three big-endian float translations (gen_anim_data).
                            int joints=data[0x4f],jp=(int)U32(data,0x54);
                            if(joints<1||joints>64||jp<0||jp+joints*16>data.Length)throw new InvalidDataException("Invalid ship joints.");
                            writer.Write(joints);
                            for(int j=0;j<joints;j++) {
                                int parent=(sbyte)data[jp+j*16];
                                if(parent < -1 || parent>=j)throw new InvalidDataException("Invalid ship hierarchy.");
                                writer.Write(parent);
                                for(int axis=0;axis<3;axis++) {
                                    float value=BitConverter.ToSingle(BitConverter.GetBytes(U32(data,jp+j*16+4+axis*4)),0);
                                    if(Single.IsNaN(value)||Single.IsInfinity(value)||Math.Abs(value)>65536)throw new InvalidDataException("Invalid ship pivot.");
                                    writer.Write(value);
                                }
                            }
                            int[] limbs=new int[nv];
                            for(int b=0;b<nb;b++) {
                                int p=bp+b*16;if((U32(data,p+12)&0x400)!=0)continue;
                                int limb=(sbyte)data[p+1],first=U16(data,p+6),end=U16(data,p+22);
                                if(limb<0||limb>=joints||first>end||end>nv)throw new InvalidDataException("Invalid ship limb.");
                                for(int v=first;v<end;v++)limbs[v]=limb;
                            }
                            writer.Write(nv);
                            for(int v=0;v<nv;v++) {
                                for(int axis=0;axis<3;axis++)writer.Write((float)(short)U16(data,vp+v*10+axis*2));
                                writer.Write(limbs[v]);
                            }
                            int faces=0;for(int b=0;b<nb;b++){int p=bp+b*16;if((U32(data,p+12)&0x400)!=0)continue;int first=U16(data,p+8),end=U16(data,p+24);if(first>end||end>nt)throw new InvalidDataException("Invalid background faces.");faces+=end-first;}
                            writer.Write(faces);
                            for(int b=0;b<nb;b++) {
                                int p=bp+b*16;if((U32(data,p+12)&0x400)!=0)continue;int material=data[p],vertexBase=U16(data,p+6),first=U16(data,p+8),end=U16(data,p+24);
                                if(material!=255&&material>=materials)throw new InvalidDataException("Invalid background material.");
                                for(int t=first;t<end;t++) {
                                    int face=fp+t*16;writer.Write(material==255?-1:material);
                                    for(int corner=0;corner<3;corner++) {
                                        int index=vertexBase+data[face+1+corner];if(index>=nv)throw new InvalidDataException("Invalid background vertex.");writer.Write(index);
                                        writer.Write((float)(short)U16(data,face+4+corner*4)/32);writer.Write((float)(short)U16(data,face+6+corner*4)/32);
                                    }
                                }
                            }
                        }
                    }
                });
            }
        }

        internal static Dictionary<int,Bitmap> Load(string romPath) {
            var images=new Dictionary<int,Bitmap>();var manifest=new List<string>();
            try {
                // Verify even on a cache hit. Cache data alone never authorizes art.
                using(var rom=LocalSetup.OpenVerifiedRom(romPath)) {
                    string cache=CacheDirectory;try{Directory.CreateDirectory(cache);}catch(IOException){}catch(UnauthorizedAccessException){}
                    long[] menu=Section(rom,26);
                    for(int weapon=0;weapon<15;weapon++) {
                        int item=U16(Read(rom,menu[0]+(weapon+10)*2,2),0);
                        if((item&0xc000)!=0x8000)throw new InvalidDataException("Unsupported weapon menu asset.");
                        byte[] sprite=Entry(rom,22,21,item&0x3fff);
                        if(sprite.Length<4||U16(sprite,2)<1)throw new InvalidDataException("Invalid weapon sprite.");
                        int texture=U16(sprite,0);string name="weapon-"+weapon+".png",path=Path.Combine(cache,name);
                        Bitmap image=Cached(path);bool fresh=image==null;if(fresh)image=Texture(rom,texture);images.Add(weapon,image);
                        if(fresh)WriteCache(path,delegate(string temp){image.Save(temp,ImageFormat.Png);});
                        manifest.Add("{\"id\":"+weapon+",\"sprite\":"+(item&0x3fff)+",\"texture\":"+texture+",\"file\":\""+name+"\"}");
                    }
                    // Named character-select plaques, each stored as two texture halves.
                    // The game character enum is Vela=0, Juno=1, Lupus=2.
                    int[] emblems={35347,35342,35344};
                    for(int character=0;character<3;character++) {
                        string name="character-"+character+".png",path=Path.Combine(cache,name);
                        Bitmap image=Cached(path);bool fresh=image==null;
                        if(fresh) {
                            using(var left=Texture(rom,emblems[character]))using(var right=Texture(rom,emblems[character]+1)) {
                                if(left.Height!=right.Height||left.Width+right.Width>128)throw new InvalidDataException("Invalid character plaque.");
                                image=new Bitmap(left.Width+right.Width,left.Height,PixelFormat.Format32bppArgb);
                                using(var graphics=Graphics.FromImage(image)){graphics.DrawImageUnscaled(left,0,0);graphics.DrawImageUnscaled(right,left.Width,0);}
                                // The character-select model maps these texture halves bottom to top.
                                image.RotateFlip(RotateFlipType.RotateNoneFlipY);
                            }
                        }
                        images.Add(300+character,image);
                        if(fresh)WriteCache(path,delegate(string temp){image.Save(temp,ImageFormat.Png);});
                        manifest.Add("{\"id\":"+(300+character)+",\"texture\":"+emblems[character]+",\"right_texture\":"+(emblems[character]+1)+",\"file\":\""+name+"\"}");
                    }
                    // Semantic item -> verified object-definition identifiers.
                    // Internal tri-rocket flag (ID 10) is excluded from physical inventory.
                    int[] itemIds={0,1,2,3,9,16,17,20,21,22,23,24,25,26,27};
                    int[] objects={118,119,120,121,127,513,508,539,509,534,438,437,436,586,585};
                    int[] ship={610,611,612,613,614,615,617,618,619,620,621,622};
                    for(int n=0;n<objects.Length+ship.Length;n++) {
                        int key=n<objects.Length?100+itemIds[n]:200+n-objects.Length,objectId=n<objects.Length?objects[n]:ship[n-objects.Length];
                        byte[] definition=Entry(rom,46,47,objectId);
                        if(definition.Length<52)throw new InvalidDataException("Invalid item object.");
                        uint modelTable=U32(definition,48);
                        if(modelTable>definition.Length-4)throw new InvalidDataException("Invalid item model table.");
                        int model=(int)U32(definition,(int)modelTable);
                        string name="item-"+key+".png",path=Path.Combine(cache,name);Bitmap image=Cached(path);bool fresh=image==null;
                        if(fresh)image=InventoryModel.Render(Inflate(Entry(rom,38,39,model),0),delegate(int texture){return Texture(rom,texture);},key==121?1:-1);
                        images.Add(key,image);if(fresh)WriteCache(path,delegate(string temp){image.Save(temp,ImageFormat.Png);});
                        manifest.Add("{\"id\":"+key+",\"object\":"+objectId+",\"model\":"+model+",\"file\":\""+name+"\"}");
                    }
                    string receipt="{\"rom_sha1\":\""+LocalSetup.RomSha1+"\",\"decoder\":\""+Version+"\",\"assets\":["+String.Join(",",manifest)+"]}";
                    WriteCache(Path.Combine(cache,"manifest.json"),delegate(string temp){File.WriteAllText(temp,receipt,new UTF8Encoding(false));});
                }
                return images;
            }catch{foreach(var image in images.Values)image.Dispose();throw;}
        }
    }
}
