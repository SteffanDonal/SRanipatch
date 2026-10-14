using Microsoft.Extensions.Logging;
using VRCFaceTracking;
using VRCFaceTracking.Core.Library;
using VRCFaceTracking.Core.Params.Data;
using VRCFaceTracking.Core.Params.Expressions;
using VRCFaceTracking.Core.Types;

namespace SRanipalExtTrackingInterface;

public sealed class SRanipalExtTrackingInterface : ExtTrackingModule
{
    private OscReceiver? _receiver;

    public override (bool SupportsEye, bool SupportsExpression) Supported => (false, true);

    public override (bool eyeSuccess, bool expressionSuccess) Initialize(
        bool eyeAvailable,
        bool expressionAvailable)
    {
        var assembly = System.Reflection.Assembly.GetExecutingAssembly();
        var image = assembly.GetManifestResourceStream(
            "SRanipalExtTrackingModule.Assets.vive_face_tracker.png");

        ModuleInformation = new ModuleMetadata
        {
            Name = "SRAnipal on Frame (OSC)",
            StaticImages = image is null ? new List<Stream>() : new List<Stream> { image },
        };

        if (!expressionAvailable)
            return (false, false);

        try
        {
            _receiver = new OscReceiver();
            Logger.LogInformation(
                "Listening for lip tracking at {Endpoint} {Address}",
                OscReceiver.Endpoint,
                OscReceiver.Address);
            return (false, true);
        }
        catch (Exception exception)
        {
            Logger.LogError(exception, "Could not start the lip tracking OSC receiver");
            return (false, false);
        }
    }

    public override void Teardown()
    {
        _receiver?.Dispose();
        _receiver = null;
    }

    public override void Update()
    {
        Thread.Sleep(10);

        if (Status != ModuleState.Active || _receiver is null)
            return;

        if (_receiver.TryReceive(out var parameters))
            UpdateMouthExpressions(ref UnifiedTracking.Data, parameters);
    }

    private static void UpdateMouthExpressions(ref UnifiedTrackingData data, float[] value)
    {
        float Get(LipParameter parameter) => value[(int)parameter];

        data.Shapes[(int)UnifiedExpressions.JawOpen].Weight =
            Get(LipParameter.JawOpen) + Get(LipParameter.MouthApeShape);
        data.Shapes[(int)UnifiedExpressions.JawLeft].Weight = Get(LipParameter.JawLeft);
        data.Shapes[(int)UnifiedExpressions.JawRight].Weight = Get(LipParameter.JawRight);
        data.Shapes[(int)UnifiedExpressions.JawForward].Weight = Get(LipParameter.JawForward);
        data.Shapes[(int)UnifiedExpressions.MouthClosed].Weight = Get(LipParameter.MouthApeShape);

        data.Shapes[(int)UnifiedExpressions.MouthUpperUpRight].Weight =
            Get(LipParameter.MouthUpperUpRight) - Get(LipParameter.MouthUpperOverturn);
        data.Shapes[(int)UnifiedExpressions.MouthUpperDeepenRight].Weight =
            Get(LipParameter.MouthUpperUpRight) - Get(LipParameter.MouthUpperOverturn);
        data.Shapes[(int)UnifiedExpressions.MouthUpperUpLeft].Weight =
            Get(LipParameter.MouthUpperUpLeft) - Get(LipParameter.MouthUpperOverturn);
        data.Shapes[(int)UnifiedExpressions.MouthUpperDeepenLeft].Weight =
            Get(LipParameter.MouthUpperUpLeft) - Get(LipParameter.MouthUpperOverturn);

        data.Shapes[(int)UnifiedExpressions.MouthLowerDownLeft].Weight =
            Get(LipParameter.MouthLowerDownLeft) - Get(LipParameter.MouthLowerOverturn);
        data.Shapes[(int)UnifiedExpressions.MouthLowerDownRight].Weight =
            Get(LipParameter.MouthLowerDownRight) - Get(LipParameter.MouthLowerOverturn);

        data.Shapes[(int)UnifiedExpressions.LipPuckerUpperLeft].Weight = Get(LipParameter.MouthPout);
        data.Shapes[(int)UnifiedExpressions.LipPuckerLowerLeft].Weight = Get(LipParameter.MouthPout);
        data.Shapes[(int)UnifiedExpressions.LipPuckerUpperRight].Weight = Get(LipParameter.MouthPout);
        data.Shapes[(int)UnifiedExpressions.LipPuckerLowerRight].Weight = Get(LipParameter.MouthPout);

        data.Shapes[(int)UnifiedExpressions.LipFunnelUpperLeft].Weight =
            Get(LipParameter.MouthUpperOverturn);
        data.Shapes[(int)UnifiedExpressions.LipFunnelUpperRight].Weight =
            Get(LipParameter.MouthUpperOverturn);
        data.Shapes[(int)UnifiedExpressions.LipFunnelLowerLeft].Weight =
            Get(LipParameter.MouthUpperOverturn);
        data.Shapes[(int)UnifiedExpressions.LipFunnelLowerRight].Weight =
            Get(LipParameter.MouthUpperOverturn);

        data.Shapes[(int)UnifiedExpressions.LipSuckUpperLeft].Weight = Get(LipParameter.MouthUpperInside);
        data.Shapes[(int)UnifiedExpressions.LipSuckUpperRight].Weight = Get(LipParameter.MouthUpperInside);
        data.Shapes[(int)UnifiedExpressions.LipSuckLowerLeft].Weight = Get(LipParameter.MouthLowerInside);
        data.Shapes[(int)UnifiedExpressions.LipSuckLowerRight].Weight = Get(LipParameter.MouthLowerInside);

        data.Shapes[(int)UnifiedExpressions.MouthUpperLeft].Weight = Get(LipParameter.MouthUpperLeft);
        data.Shapes[(int)UnifiedExpressions.MouthUpperRight].Weight = Get(LipParameter.MouthUpperRight);
        data.Shapes[(int)UnifiedExpressions.MouthLowerLeft].Weight = Get(LipParameter.MouthLowerLeft);
        data.Shapes[(int)UnifiedExpressions.MouthLowerRight].Weight = Get(LipParameter.MouthLowerRight);

        data.Shapes[(int)UnifiedExpressions.MouthCornerPullLeft].Weight = Get(LipParameter.MouthSmileLeft);
        data.Shapes[(int)UnifiedExpressions.MouthCornerPullRight].Weight = Get(LipParameter.MouthSmileRight);
        data.Shapes[(int)UnifiedExpressions.MouthCornerSlantLeft].Weight = Get(LipParameter.MouthSmileLeft);
        data.Shapes[(int)UnifiedExpressions.MouthCornerSlantRight].Weight = Get(LipParameter.MouthSmileRight);
        data.Shapes[(int)UnifiedExpressions.MouthFrownLeft].Weight = Get(LipParameter.MouthSadLeft);
        data.Shapes[(int)UnifiedExpressions.MouthFrownRight].Weight = Get(LipParameter.MouthSadRight);

        data.Shapes[(int)UnifiedExpressions.MouthRaiserUpper].Weight =
            Get(LipParameter.MouthLowerOverlay) - Get(LipParameter.MouthUpperInside);
        data.Shapes[(int)UnifiedExpressions.MouthRaiserLower].Weight = Get(LipParameter.MouthLowerOverlay);

        data.Shapes[(int)UnifiedExpressions.CheekPuffLeft].Weight = Get(LipParameter.CheekPuffLeft);
        data.Shapes[(int)UnifiedExpressions.CheekPuffRight].Weight = Get(LipParameter.CheekPuffRight);
        data.Shapes[(int)UnifiedExpressions.CheekSuckLeft].Weight = Get(LipParameter.CheekSuck);
        data.Shapes[(int)UnifiedExpressions.CheekSuckRight].Weight = Get(LipParameter.CheekSuck);

        data.Shapes[(int)UnifiedExpressions.TongueOut].Weight =
            (Get(LipParameter.TongueLongStep1) + Get(LipParameter.TongueLongStep2)) / 2.0f;
        data.Shapes[(int)UnifiedExpressions.TongueUp].Weight = Get(LipParameter.TongueUp);
        data.Shapes[(int)UnifiedExpressions.TongueDown].Weight = Get(LipParameter.TongueDown);
        data.Shapes[(int)UnifiedExpressions.TongueLeft].Weight = Get(LipParameter.TongueLeft);
        data.Shapes[(int)UnifiedExpressions.TongueRight].Weight = Get(LipParameter.TongueRight);
        data.Shapes[(int)UnifiedExpressions.TongueRoll].Weight = Get(LipParameter.TongueRoll);

        data.Shapes[(int)UnifiedExpressions.CheekSquintLeft].Weight = Get(LipParameter.MouthSmileLeft);
        data.Shapes[(int)UnifiedExpressions.CheekSquintRight].Weight = Get(LipParameter.MouthSmileRight);
        data.Shapes[(int)UnifiedExpressions.MouthDimpleLeft].Weight = Get(LipParameter.MouthSmileLeft);
        data.Shapes[(int)UnifiedExpressions.MouthDimpleRight].Weight = Get(LipParameter.MouthSmileRight);
        data.Shapes[(int)UnifiedExpressions.MouthStretchLeft].Weight = Get(LipParameter.MouthSadRight);
        data.Shapes[(int)UnifiedExpressions.MouthStretchRight].Weight = Get(LipParameter.MouthSadRight);
    }
}
