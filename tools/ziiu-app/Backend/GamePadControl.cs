using System.Net.Sockets;
using System.Text;

namespace ZiiuApp.Backend;

// The running stream's local control ports, the same datagrams the scripts send:
//   ziiu-drc   127.0.0.1:50200  rumble (level, u16 LE ms), "ZIR1"/"ZIR0"/"ZIRT"  (rumble.ps1, ir.ps1)
//   ziiu-input 127.0.0.1:50222  "ZCALIBRATE"                                       (calibrate.ps1)
// UDP gives no reply; results show up in encoder.log / input.log.
public static class GamePadControl
{
    const int EncoderPort = 50200, InputPort = 50222;

    public static void Calibrate() => Send(InputPort, Encoding.ASCII.GetBytes("ZCALIBRATE"));

    public static void SensorBar(bool? on) =>
        Send(EncoderPort, Encoding.ASCII.GetBytes(on == null ? "ZIRT" : on.Value ? "ZIR1" : "ZIR0"));

    public static void Rumble(int level, int ms) =>
        Send(EncoderPort, [(byte)level, (byte)(ms & 0xff), (byte)((ms >> 8) & 0xff)]);

    static void Send(int port, byte[] data)
    {
        using var udp = new UdpClient();
        udp.Send(data, data.Length, "127.0.0.1", port);
    }
}
