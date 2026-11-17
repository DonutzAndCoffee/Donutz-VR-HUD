using System;
using System.Drawing;

namespace Donutz.SimHubLedBridge
{
    public static class LedPacket
    {
        public static byte[] Encode(Color[] colors)
        {
            if (colors == null) throw new ArgumentNullException(nameof(colors));
            var packet = new byte[8 + 256 * 3];
            packet[0] = (byte)'D'; packet[1] = (byte)'L'; packet[2] = (byte)'E'; packet[3] = (byte)'D';
            packet[4] = 1;
            packet[6] = 0; packet[7] = 1;
            for (var i = 0; i < Math.Min(colors.Length, 256); i++)
            {
                var c = colors[i];
                packet[8 + i * 3] = (byte)(c.R * c.A / 255);
                packet[9 + i * 3] = (byte)(c.G * c.A / 255);
                packet[10 + i * 3] = (byte)(c.B * c.A / 255);
            }
            return packet;
        }

        public static byte[] EncodeMotionPacket(float heaveG, bool telemetryActive)
        {
            var packet = new byte[10];
            packet[0] = (byte)'D'; packet[1] = (byte)'M'; packet[2] = (byte)'O'; packet[3] = (byte)'T';
            packet[4] = 2;
            packet[5] = telemetryActive ? (byte)1 : (byte)0;
            Buffer.BlockCopy(BitConverter.GetBytes(heaveG), 0, packet, 6, 4);
            return packet;
        }
    }
}
