$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
Add-Type -TypeDefinition @'
using System;
using System.Net.Sockets;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
public static class PowerTrial {
 static object gate=new object();static string host="192.168.137.58";
 static string F(double x){return x.ToString("F4",System.Globalization.CultureInfo.InvariantCulture);}
 static void Log(string x){lock(gate)Console.WriteLine(x);}
 static TcpClient Connect(){var c=new TcpClient();c.NoDelay=true;c.SendTimeout=3000;c.ReceiveTimeout=3000;var a=c.BeginConnect(host,5210,null,null);if(!a.AsyncWaitHandle.WaitOne(3000)){c.Close();throw new Exception("connect timeout");}c.EndConnect(a);return c;}
 static void Exact(NetworkStream s,byte[] b){int i=0;while(i<b.Length){int n=s.Read(b,i,b.Length-i);if(n==0)throw new Exception("EOF");i+=n;}}
 static void Echo(NetworkStream s,byte[] b,byte[] got){s.Write(BitConverter.GetBytes(b.Length),0,4);s.Write(b,0,b.Length);Exact(s,got);for(int i=0;i<b.Length;i++)if(b[i]!=got[i])throw new Exception("data mismatch");}
 static void Work(int phase,int id,double duration,bool idle){var w=Stopwatch.StartNew();long count=0,bytes=0;int errors=0;byte[] b=new byte[idle?32:65536],got=new byte[b.Length];new Random(1701+id).NextBytes(b);
 while(w.Elapsed.TotalSeconds<duration){try{using(var c=Connect()){var s=c.GetStream();s.WriteByte((byte)'E');while(w.Elapsed.TotalSeconds<duration){Array.Copy(BitConverter.GetBytes(count),b,8);var q=Stopwatch.StartNew();Echo(s,b,got);count++;bytes+=b.Length;if(idle){Log("{\"kind\":\"rtt\",\"phase\":"+phase+",\"ms\":"+F(q.Elapsed.TotalMilliseconds)+"}");Thread.Sleep(200);}}}}catch(Exception e){errors++;Log("{\"kind\":\"error\",\"phase\":"+phase+",\"worker\":"+id+",\"type\":\""+e.GetType().Name+"\",\"message\":\""+e.Message.Replace("\\","/").Replace("\"","'").Replace("\r"," ").Replace("\n"," ")+"\"}");}}
 Log("{\"kind\":\"worker\",\"phase\":"+phase+",\"idle\":"+(idle?"true":"false")+",\"id\":"+id+",\"elapsed_s\":"+F(w.Elapsed.TotalSeconds)+",\"bytes_each_direction\":"+bytes+",\"blocks\":"+count+",\"errors\":"+errors+"}");}
 public static void Run(){ThreadPool.SetMinThreads(16,16);try{for(int phase=0;phase<4;phase++){int ps=phase%2==0?1:0;using(var c=Connect()){var s=c.GetStream();s.WriteByte((byte)'P');s.WriteByte((byte)ps);int actual=s.ReadByte();if(actual!=ps)throw new Exception("power readback mismatch");}Log("{\"kind\":\"phase\",\"phase\":"+phase+",\"power\":"+ps+",\"utc\":\""+DateTime.UtcNow.ToString("o")+"\"}");Thread.Sleep(1000);Work(phase,0,15,true);var w=Stopwatch.StartNew();var tasks=new Task[4];for(int j=0;j<4;j++){int id=j;int ph=phase;tasks[j]=Task.Run(()=>Work(ph,id,45,false));}Task.WaitAll(tasks);Log("{\"kind\":\"load_end\",\"phase\":"+phase+",\"elapsed_s\":"+F(w.Elapsed.TotalSeconds)+"}");}}finally{using(var c=Connect()){c.GetStream().WriteByte((byte)'Q');}}}
}
'@
[PowerTrial]::Run()
