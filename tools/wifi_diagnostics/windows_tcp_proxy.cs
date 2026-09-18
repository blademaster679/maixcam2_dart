using System;
using System.Net.Sockets;
using System.Threading.Tasks;
public static class MaixWifiProxy {
 public static int Main(string[] args) {
  try {
   if(args.Length!=2) throw new ArgumentException("host and port required");
   using(var client=new TcpClient()) {
    var pending=client.BeginConnect(args[0],int.Parse(args[1]),null,null);
    if(!pending.AsyncWaitHandle.WaitOne(15000))throw new TimeoutException("TCP connect timed out");
    client.EndConnect(pending);
    using(var stream=client.GetStream()) {
     var send=Console.OpenStandardInput().CopyToAsync(stream);
     var recv=stream.CopyToAsync(Console.OpenStandardOutput());
     Task.WhenAny(send,recv).GetAwaiter().GetResult().GetAwaiter().GetResult();
    }
   }
   return 0;
  } catch(Exception e) { Console.Error.WriteLine(e.Message);return 1; }
 }
}
