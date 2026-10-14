using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace SRanipalExtTrackingInterface;

internal sealed class OscReceiver : IDisposable
{
    public const string Address = "/sranipal/parameters";
    public const string Endpoint = "127.0.0.1:9000";

    private const int ParameterCount = 37;
    private readonly Socket _socket;
    private readonly byte[] _packet = new byte[2048];
    private readonly float[] _latest = new float[ParameterCount];

    public OscReceiver()
    {
        _socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            _socket.Blocking = false;
            _socket.Bind(new IPEndPoint(IPAddress.Loopback, 9000));
        }
        catch
        {
            _socket.Dispose();
            throw;
        }
    }

    public bool TryReceive(out float[] parameters)
    {
        var receivedValidPacket = false;

        while (_socket.Available > 0)
        {
            int length;
            try
            {
                length = _socket.Receive(_packet, SocketFlags.None);
            }
            catch (SocketException exception) when (exception.SocketErrorCode == SocketError.WouldBlock)
            {
                break;
            }

            if (TryParse(_packet.AsSpan(0, length), _latest))
                receivedValidPacket = true;
        }

        parameters = _latest;
        return receivedValidPacket;
    }

    public void Dispose() => _socket.Dispose();

    internal static bool TryParse(ReadOnlySpan<byte> packet, Span<float> destination)
    {
        if (destination.Length < ParameterCount)
            return false;

        var offset = 0;
        if (!TryReadString(packet, ref offset, out var address) || address != Address)
            return false;
        if (!TryReadString(packet, ref offset, out var types) ||
            types.Length != ParameterCount + 1 ||
            types[0] != ',')
            return false;
        for (var index = 1; index < types.Length; ++index)
        {
            if (types[index] != 'f')
                return false;
        }
        if (packet.Length != offset + ParameterCount * sizeof(float))
            return false;

        Span<float> parsed = stackalloc float[ParameterCount];
        for (var index = 0; index < ParameterCount; ++index)
        {
            var bits = BinaryPrimitives.ReadInt32BigEndian(packet.Slice(offset, sizeof(float)));
            var value = BitConverter.Int32BitsToSingle(bits);
            if (!float.IsFinite(value))
                return false;
            parsed[index] = value;
            offset += sizeof(float);
        }

        parsed.CopyTo(destination);
        return true;
    }

    private static bool TryReadString(ReadOnlySpan<byte> packet, ref int offset, out string value)
    {
        value = string.Empty;
        if ((uint)offset >= (uint)packet.Length)
            return false;

        var terminator = packet[offset..].IndexOf((byte)0);
        if (terminator < 0)
            return false;

        value = Encoding.ASCII.GetString(packet.Slice(offset, terminator));
        offset = (offset + terminator + 4) & ~3;
        return offset <= packet.Length;
    }
}
