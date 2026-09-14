using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text.Json;
using HIDMaestro;

namespace Sunshine.Ds5Sidecar;

internal static class ProtocolSelfTest
{
    internal static async Task RunHostProtocolChecksAsync()
    {
        var pipeName = $"sunshine-device-host-contract-{Environment.ProcessId}-{Guid.NewGuid():N}";
        using var stopping = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await using var server = new VirtualDeviceHostServer(
            pipeName, enableCompositeMicrophonePrototype: false,
            skipOwnerVerificationForTests: true);
        var serverTask = server.RunAsync(stopping.Token);

        await using var client = new NamedPipeClientStream(
            ".", pipeName, PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.WriteThrough);
        await client.ConnectAsync(5_000, stopping.Token);

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Hello, 1, new byte[4]), stopping.Token);
        var hello = await ReceiveUntilAsync(
            client, Protocol.MessageType.HelloReply, 1, stopping.Token);
        var capabilities = (Protocol.Capability)BinaryPrimitives.ReadUInt32LittleEndian(hello.Payload);
        const Protocol.Capability microphoneCapabilities =
            Protocol.Capability.VirtualMicrophone |
            Protocol.Capability.PersistentDeviceHost |
            Protocol.Capability.MicrophoneStatus;
        Require((capabilities & microphoneCapabilities) == 0,
            "host protocol default-off microphone capabilities");

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.MicCreate, 2,
            new byte[] { 0x80, 0xbb, 0, 0, 1, 16, 0, 0 }), stopping.Token);
        var create = await ReceiveUntilAsync(
            client, Protocol.MessageType.MicCreateReply, 2, stopping.Token);
        Require(BinaryPrimitives.ReadInt32LittleEndian(create.Payload.AsSpan(0, 4)) ==
                (int)Protocol.MicrophoneResult.TransportUnavailable,
            "host protocol disabled microphone create");

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.MicFlush, 3, Array.Empty<byte>()), stopping.Token);
        var flush = await ReceiveUntilAsync(
            client, Protocol.MessageType.MicFlushReply, 3, stopping.Token);
        Require(BinaryPrimitives.ReadInt32LittleEndian(flush.Payload.AsSpan(0, 4)) ==
                (int)Protocol.MicrophoneResult.DeviceNotCreated,
            "host protocol absent microphone flush");

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.MicDestroy, 4, Array.Empty<byte>()), stopping.Token);
        var destroy = await ReceiveUntilAsync(
            client, Protocol.MessageType.MicDestroyReply, 4, stopping.Token);
        Require(BinaryPrimitives.ReadInt32LittleEndian(destroy.Payload.AsSpan(0, 4)) ==
                (int)Protocol.MicrophoneResult.DeviceNotCreated,
            "host protocol absent microphone destroy");

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.MicPcm, 5, Array.Empty<byte>()), stopping.Token);
        var invalidRequestId = await ReceiveAsync(client, stopping.Token);
        Require(invalidRequestId.Type == Protocol.MessageType.Error &&
                invalidRequestId.RequestId == 5,
            "host protocol microphone PCM request id rejection");

        client.Dispose();
        await serverTask.WaitAsync(TimeSpan.FromSeconds(5));
    }

    internal static async Task<int> RunMicrophonePrototypeAsync(
        string? resultPath, bool requireHostCapture = false)
    {
        if (!DeviceRegistry.HasPinnedMicrophoneRuntime)
            throw new InvalidOperationException(
                "The composite microphone prototype requires the pinned HIDMaestro 1.7.3.0 runtime");

        var pipeName = $"sunshine-microphone-prototype-{Environment.ProcessId}-{Guid.NewGuid():N}";
        using var stopping = new CancellationTokenSource(TimeSpan.FromSeconds(45));
        await using var server = new VirtualDeviceHostServer(
            pipeName, enableCompositeMicrophonePrototype: true);
        var serverTask = server.RunAsync(stopping.Token);
        await using var client = new NamedPipeClientStream(
            ".", pipeName, PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.WriteThrough);
        await client.ConnectAsync(10_000, stopping.Token);

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Hello, 1, new byte[4]), stopping.Token);
        var hello = await ReceiveUntilAsync(
            client, Protocol.MessageType.HelloReply, 1, stopping.Token);
        var capabilities = (Protocol.Capability)BinaryPrimitives.ReadUInt32LittleEndian(hello.Payload);
        Require(capabilities.HasFlag(Protocol.Capability.VirtualMicrophone) &&
                capabilities.HasFlag(Protocol.Capability.PersistentDeviceHost) &&
                capabilities.HasFlag(Protocol.Capability.MicrophoneStatus),
            "microphone prototype capabilities");

        var captureEndpointsBefore = requireHostCapture
            ? WasapiCaptureProbe.GetActiveCaptureEndpointIds()
            : null;

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.MicCreate, 2,
            new byte[] { 0x80, 0xbb, 0, 0, 1, 16, 0, 0 }), stopping.Token);
        var create = await ReceiveUntilAsync(
            client, Protocol.MessageType.MicCreateReply, 2, stopping.Token);
        var result = BinaryPrimitives.ReadInt32LittleEndian(create.Payload.AsSpan(0, 4));
        Require(result == (int)Protocol.MicrophoneResult.Success,
            "microphone prototype create");
        var generation = BinaryPrimitives.ReadUInt32LittleEndian(create.Payload.AsSpan(4, 4));
        Require(generation != 0, "microphone prototype generation");

        string? captureEndpointId = null;
        if (requireHostCapture)
        {
            string[] addedEndpoints = Array.Empty<string>();
            for (var attempt = 0; attempt < 50 && addedEndpoints.Length == 0; ++attempt)
            {
                addedEndpoints = WasapiCaptureProbe.GetActiveCaptureEndpointIds()
                    .Except(captureEndpointsBefore!, StringComparer.OrdinalIgnoreCase)
                    .ToArray();
                if (addedEndpoints.Length == 0)
                    await Task.Delay(100, stopping.Token);
            }
            Require(addedEndpoints.Length == 1,
                "microphone prototype unique WASAPI capture endpoint");
            captureEndpointId = addedEndpoints[0];
        }

        var packetCount = requireHostCapture ? 500u : 2u;
        var hostStreamingSeen = false;
        uint submitErrors = 0;
        ulong capturedFrames = 0;
        ulong nonZeroFrames = 0;
        if (requireHostCapture)
        {
            var captureResult = await WasapiCaptureProbe.RunOnDedicatedThreadAsync(
                captureEndpointId!, probe =>
                {
                    for (uint sequence = 0; sequence < packetCount; ++sequence)
                    {
                        var payload = CreateMicrophoneTestPcmPayload(
                            generation, sequence, packetCount);
                        SendAsync(client, new Protocol.Message(
                            Protocol.MessageType.MicPcm, 0, payload), stopping.Token)
                            .GetAwaiter().GetResult();
                        Task.Delay(10, stopping.Token).GetAwaiter().GetResult();
                        probe.Drain();
                    }
                    probe.Drain();

                    var streamingSeen = false;
                    uint errors = 0;
                    SendAsync(client, new Protocol.Message(
                        Protocol.MessageType.MicFlush, 3, Array.Empty<byte>()), stopping.Token)
                        .GetAwaiter().GetResult();
                    var flushReply = ReceiveUntilAsync(
                        client, Protocol.MessageType.MicFlushReply, 3, stopping.Token,
                        message => ObserveMicrophoneStatus(
                            message, ref streamingSeen, ref errors))
                        .GetAwaiter().GetResult();
                    return (Flush: flushReply, HostStreamingSeen: streamingSeen,
                        SubmitErrors: errors, probe.CapturedFrames, probe.NonZeroFrames);
                });
            Require(BinaryPrimitives.ReadInt32LittleEndian(
                    captureResult.Flush.Payload.AsSpan(0, 4)) ==
                    (int)Protocol.MicrophoneResult.Success,
                "microphone prototype flush");
            hostStreamingSeen = captureResult.HostStreamingSeen;
            submitErrors = captureResult.SubmitErrors;
            capturedFrames = captureResult.CapturedFrames;
            nonZeroFrames = captureResult.NonZeroFrames;
            Require(hostStreamingSeen, "microphone prototype host capture pin");
            Require(submitErrors == 0, "microphone prototype host capture submissions");
            Require(capturedFrames != 0,
                "microphone prototype WASAPI captured frames");
            Require(nonZeroFrames != 0,
                "microphone prototype WASAPI non-zero frames");
        }
        else
        {
            for (uint sequence = 0; sequence < packetCount; ++sequence)
            {
                var payload = CreateMicrophoneTestPcmPayload(
                    generation, sequence, packetCount);
                await SendAsync(client, new Protocol.Message(
                    Protocol.MessageType.MicPcm, 0, payload), stopping.Token);
            }

            await SendAsync(client, new Protocol.Message(
                Protocol.MessageType.MicFlush, 3, Array.Empty<byte>()), stopping.Token);
            var flush = await ReceiveUntilAsync(
                client, Protocol.MessageType.MicFlushReply, 3, stopping.Token,
                message => ObserveMicrophoneStatus(
                    message, ref hostStreamingSeen, ref submitErrors));
            Require(BinaryPrimitives.ReadInt32LittleEndian(flush.Payload.AsSpan(0, 4)) ==
                    (int)Protocol.MicrophoneResult.Success,
                "microphone prototype flush");
        }

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.MicDestroy, 4, Array.Empty<byte>()), stopping.Token);
        var destroy = await ReceiveUntilAsync(
            client, Protocol.MessageType.MicDestroyReply, 4, stopping.Token);
        Require(BinaryPrimitives.ReadInt32LittleEndian(destroy.Payload.AsSpan(0, 4)) ==
                (int)Protocol.MicrophoneResult.Success,
            "microphone prototype destroy");

        client.Dispose();
        await serverTask.WaitAsync(TimeSpan.FromSeconds(10));
        var output = JsonSerializer.Serialize(new
        {
            protocol = Protocol.Version,
            profile = DualSenseHapticsAudio.CompositeProfileId,
            virtual_microphone = true,
            generation,
            pcm_frames = packetCount * 480u,
            host_streaming_seen = hostStreamingSeen,
            submit_errors = submitErrors,
            wasapi_captured_frames = capturedFrames,
            wasapi_nonzero_frames = nonZeroFrames,
            flushed = true,
            destroyed = true,
        });
        Console.WriteLine(output);
        if (!string.IsNullOrWhiteSpace(resultPath))
            await File.WriteAllTextAsync(resultPath, output, stopping.Token);
        return 0;
    }

    private static byte[] CreateMicrophoneTestPcmPayload(
        uint generation, uint sequence, uint packetCount)
    {
        var flags = sequence == 0
            ? Protocol.MicPcmFlags.StreamStart
            : Protocol.MicPcmFlags.None;
        if (sequence + 1 == packetCount)
            flags |= Protocol.MicPcmFlags.StreamEnd;
        var payload = CreateMicPcmPayload(
            generation, sequence, sequence * 10_000, 480, flags);
        for (var frame = 0; frame < 480; ++frame)
        {
            var sampleIndex = sequence * 480u + (uint)frame;
            var sample = (short)(Math.Sin(
                sampleIndex * 2.0 * Math.PI * 440.0 / 48_000.0) * 4_096);
            BinaryPrimitives.WriteInt16LittleEndian(
                payload.AsSpan(Protocol.MicPcmHeaderSize + frame * 2, 2), sample);
        }
        return payload;
    }

    private static void ObserveMicrophoneStatus(
        Protocol.Message message, ref bool hostStreamingSeen, ref uint submitErrors)
    {
        if (message.Type != Protocol.MessageType.MicStatus ||
            message.Payload.Length != Protocol.MicStatusPayloadSize)
            return;
        hostStreamingSeen |= message.Payload[5] != 0;
        submitErrors = Math.Max(submitErrors,
            BinaryPrimitives.ReadUInt32LittleEndian(message.Payload.AsSpan(20, 4)));
    }

    internal static async Task<int> RunAsync(bool composite, string? resultPath, string? audioWriterPath)
    {
        RunDeterministicChecks();
        VerifyAdaptiveTriggerEncoding();
        var pipeName = $"sunshine-ds5-self-test-{Environment.ProcessId}-{Guid.NewGuid():N}";
        using var stopping = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        await using var server = new VirtualDeviceHostServer(pipeName);
        var serverTask = server.RunAsync(stopping.Token);

        await using var client = new NamedPipeClientStream(
            ".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.WriteThrough);
        await client.ConnectAsync(10_000, stopping.Token);

        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Hello, 1, new byte[4]), stopping.Token);
        var hello = await ReceiveAsync(client, stopping.Token);
        Require(hello.Type == Protocol.MessageType.HelloReply && hello.RequestId == 1 && hello.Payload.Length == 4,
            "hello reply");
        var helloCapabilities = (Protocol.Capability)BinaryPrimitives.ReadUInt32LittleEndian(hello.Payload);
        Require(helloCapabilities.HasFlag(Protocol.Capability.Hid), "hello HID capability");
        Require(helloCapabilities.HasFlag(Protocol.Capability.AdaptiveTriggers), "hello adaptive trigger capability");
        Require(!composite || helloCapabilities.HasFlag(Protocol.Capability.GenshinCompatibilityIdentity),
            "hello Genshin compatibility identity capability");
        Require(helloCapabilities.HasFlag(Protocol.Capability.AudioPolicyViolation),
            "hello audio endpoint policy capability");

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Attach, 2, new byte[] { 0, 0, composite ? (byte)1 : (byte)0, 0 }), stopping.Token);
        var attach = await ReceiveAsync(client, stopping.Token);
        if (attach.Type == Protocol.MessageType.Error)
            throw new InvalidOperationException(DecodeError(attach.Payload));
        Require(attach.Type == Protocol.MessageType.AttachReply && attach.RequestId == 2 && attach.Payload.Length == 8,
            "attach reply");
        var capabilities = (Protocol.Capability)BinaryPrimitives.ReadUInt32LittleEndian(attach.Payload.AsSpan(4));
        Require(capabilities.HasFlag(Protocol.Capability.Hid), "HID capability");
        Require(capabilities.HasFlag(Protocol.Capability.AdaptiveTriggers), "adaptive trigger capability");
        Require(!composite || capabilities.HasFlag(Protocol.Capability.AudioFourChannel),
            "composite four-channel audio capability");

        var input = new byte[20];
        input[0] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(input.AsSpan(4), 0x1000 | 0x0010); // Cross + Start
        input[8] = 64;
        input[9] = 128;
        BinaryPrimitives.WriteInt16LittleEndian(input.AsSpan(12), -1234);
        BinaryPrimitives.WriteInt16LittleEndian(input.AsSpan(14), 2345);
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.InputState, 0, input), stopping.Token);

        var touch = new byte[20];
        touch[0] = 0;
        touch[1] = 1;
        BinaryPrimitives.WriteUInt32LittleEndian(touch.AsSpan(4), 42);
        WriteFloat(touch.AsSpan(8), 0.25f);
        WriteFloat(touch.AsSpan(12), 0.75f);
        WriteFloat(touch.AsSpan(16), 1.0f);
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Touch, 0, touch), stopping.Token);
        touch[1] = 2;
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Touch, 0, touch), stopping.Token);
        touch[1] = 5; // LI_TOUCH_EVENT_BUTTON_ONLY must not mutate contact state or fail.
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Touch, 0, touch), stopping.Token);

        var motion = new byte[16];
        motion[0] = 0;
        motion[1] = 1;
        WriteFloat(motion.AsSpan(4), 0.0f);
        WriteFloat(motion.AsSpan(8), 9.80665f);
        WriteFloat(motion.AsSpan(12), 0.0f);
        await SendAsync(client, new Protocol.Message(Protocol.MessageType.Motion, 0, motion), stopping.Token);

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Battery, 0, new byte[] { 0, 3, 80, 0 }), stopping.Token);

        var capturedHapticsBytes = 0;
        if (!string.IsNullOrWhiteSpace(audioWriterPath))
        {
            Require(composite, "audio writer requires composite profile");
            Require(File.Exists(audioWriterPath), "audio writer executable");
            await Task.Delay(500, stopping.Token);
            using var writer = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = audioWriterPath,
                WorkingDirectory = Path.GetDirectoryName(audioWriterPath)!,
                UseShellExecute = false,
                CreateNoWindow = true,
            }) ?? throw new InvalidOperationException("Unable to launch the haptics audio writer");
            while (capturedHapticsBytes == 0)
            {
                var haptics = await ReceiveUntilTypeAsync(client, Protocol.MessageType.HapticsPcm, stopping.Token);
                Require(haptics.Payload.Length >= 24 && haptics.Payload[3] == 2 && haptics.Payload[6] == 16,
                    "haptics PCM format");
                var frameCount = BinaryPrimitives.ReadUInt16LittleEndian(haptics.Payload.AsSpan(4));
                Require(BinaryPrimitives.ReadUInt32LittleEndian(haptics.Payload.AsSpan(20)) == 48000 &&
                        haptics.Payload.Length == 24 + frameCount * 4,
                    "haptics PCM size");
                var energy = 0L;
                for (var i = 24; i + 1 < haptics.Payload.Length; i += 2)
                    energy += Math.Abs((int)BinaryPrimitives.ReadInt16LittleEndian(haptics.Payload.AsSpan(i, 2)));
                if (energy != 0)
                    capturedHapticsBytes = haptics.Payload.Length - 24;
            }
            await writer.WaitForExitAsync(stopping.Token);
            Require(writer.ExitCode == 0, "audio writer exit code");
        }

        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Detach, 3, new byte[] { 0 }), stopping.Token);
        var detach = await ReceiveUntilAsync(client, Protocol.MessageType.DetachReply, 3, stopping.Token);
        Require(detach.Payload.Length == 1 && detach.Payload[0] == 0, "detach reply");

        // Reattach and intentionally drop the owner pipe without DETACH. Completion
        // of serverTask proves the EOF path disposed the still-attached controller.
        await SendAsync(client, new Protocol.Message(
            Protocol.MessageType.Attach, 4, new byte[] { 0, 0, composite ? (byte)1 : (byte)0, 0 }), stopping.Token);
        var ownerCleanupAttach = await ReceiveUntilAsync(
            client, Protocol.MessageType.AttachReply, 4, stopping.Token);
        Require(ownerCleanupAttach.Payload.Length == 8, "owner cleanup attach reply");

        client.Dispose();
        await serverTask.WaitAsync(TimeSpan.FromSeconds(10));

        var result = JsonSerializer.Serialize(new
        {
            protocol = Protocol.Version,
            profile = composite ? "dualsense-composite" : "dualsense-hidonly",
            hello = true,
            attached = true,
            four_channel_audio = capabilities.HasFlag(Protocol.Capability.AudioFourChannel),
            input = true,
            touch = true,
            motion = true,
            battery = true,
            adaptive_triggers = true,
            haptics_pcm = capturedHapticsBytes != 0,
            haptics_bytes = capturedHapticsBytes,
            detached = true,
            owner_disconnect_cleanup = true,
            cleanup = true,
        });
        Console.WriteLine(result);
        if (!string.IsNullOrWhiteSpace(resultPath))
            await File.WriteAllTextAsync(resultPath, result, stopping.Token);
        return 0;
    }

    internal static void RunDeterministicChecks()
    {
        VerifyProtocolAbi();
        VerifyHidMaestroMicrophoneContract();
        VerifyMicrophonePcmQueue();
        VerifyVirtualMicrophoneSession();
        VerifyMicrophoneRegistryGating();
        VerifyBundledCompositeProfile();
        VerifyProfileSelection();
        VerifyHapticsChannelIsolation();
        VerifyDefaultAudioEndpointClassification();
        VerifyDefaultAudioEndpointPolicy();
        VerifyControllerStateSubmissionPolicy();
        VerifySensorTimestampEncoding();
        VerifyOutputValidityFlags();
        VerifyOutputValidityGating();
    }

    private static void VerifyHidMaestroMicrophoneContract()
    {
        var microphoneType = typeof(HMMicrophoneInput);
        var submit = microphoneType.GetMethod(
            "Submit", new[] { typeof(ReadOnlySpan<byte>) });
        Require(submit is not null && submit.ReturnType == typeof(int),
            "HIDMaestro microphone Submit(ReadOnlySpan<byte>) contract");

        foreach (var (name, propertyType) in new[]
                 {
                     ("Channels", typeof(int)),
                     ("SampleRateHz", typeof(int)),
                     ("BitsPerSample", typeof(int)),
                     ("IsStreaming", typeof(bool)),
                     ("BufferedBytes", typeof(int)),
                 })
        {
            var property = microphoneType.GetProperty(name);
            Require(property?.GetMethod?.IsPublic == true &&
                    property.PropertyType == propertyType,
                $"HIDMaestro microphone {name} contract");
        }

        var streamingChanged = microphoneType.GetEvent("StreamingChanged");
        Require(streamingChanged?.EventHandlerType == typeof(Action<HMMicrophoneInput, bool>),
            "HIDMaestro microphone StreamingChanged contract");
        Require(typeof(HMUsbAudio).GetProperty("Microphone")?.PropertyType == microphoneType,
            "HIDMaestro USB audio microphone accessor contract");
    }

    private static void VerifyProtocolAbi()
    {
        Require(Protocol.Magic == 0x35534453 && Protocol.Version == 1 && Protocol.HeaderSize == 16,
            "SDS5 v1 header constants");
        Require((uint)Protocol.Capability.VirtualMicrophone == 1u << 10 &&
                (uint)Protocol.Capability.PersistentDeviceHost == 1u << 11 &&
                (uint)Protocol.Capability.MicrophoneStatus == 1u << 12,
            "virtual microphone capability bits");
        const Protocol.Capability microphoneCapabilities =
            Protocol.Capability.VirtualMicrophone |
            Protocol.Capability.PersistentDeviceHost |
            Protocol.Capability.MicrophoneStatus;
        Require((DeviceRegistry.BaseCapabilities & microphoneCapabilities) == 0,
            "unimplemented microphone capabilities remain unadvertised");
        Require((ushort)Protocol.MessageType.MicCreate == 12 &&
                (ushort)Protocol.MessageType.MicDestroyReply == 18 &&
                (ushort)Protocol.MessageType.HostStatus == 106 &&
                (ushort)Protocol.MessageType.MicStatus == 107,
            "virtual microphone message numbers");

        var createPayload = new byte[] { 0x80, 0xbb, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00 };
        var frame = Protocol.Encode(new Protocol.Message(
            Protocol.MessageType.MicCreate, 0x11223344, createPayload));
        Require(frame.SequenceEqual(new byte[]
        {
            0x53, 0x44, 0x53, 0x35, 0x01, 0x00, 0x0c, 0x00,
            0x08, 0x00, 0x00, 0x00, 0x44, 0x33, 0x22, 0x11,
            0x80, 0xbb, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00,
        }), "microphone create golden frame");
        var header = Protocol.DecodeHeader(frame.AsSpan(0, Protocol.HeaderSize));
        Require(header.Type == Protocol.MessageType.MicCreate && header.PayloadLength == 8 &&
                header.RequestId == 0x11223344,
            "microphone create golden header decode");
        var create = Protocol.DecodeMicCreate(createPayload);
        Require(create.SampleRateHz == 48_000 && create.Channels == 1 &&
                create.BitsPerSample == 16 && create.Flags == 0,
            "microphone create payload decode");

        var reply = Protocol.EncodeMicCreateReply(-7, 0x10203040);
        Require(reply.Length == Protocol.MicCreateReplyPayloadSize &&
                BinaryPrimitives.ReadInt32LittleEndian(reply.AsSpan(0, 4)) == -7 &&
                BinaryPrimitives.ReadUInt32LittleEndian(reply.AsSpan(4, 4)) == 0x10203040 &&
                BinaryPrimitives.ReadUInt32LittleEndian(reply.AsSpan(8, 4)) == 48_000 &&
                reply[12] == 1 && reply[13] == 16 && reply[14] == 0 && reply[15] == 0,
            "microphone create reply ABI");
        var operationReply = Protocol.EncodeMicOperationReply(
            Protocol.MicrophoneResult.DeviceNotCreated, 0x50607080);
        Require(operationReply.Length == Protocol.MicOperationReplyPayloadSize &&
                BinaryPrimitives.ReadInt32LittleEndian(operationReply.AsSpan(0, 4)) ==
                    (int)Protocol.MicrophoneResult.DeviceNotCreated &&
                BinaryPrimitives.ReadUInt32LittleEndian(operationReply.AsSpan(4, 4)) == 0x50607080,
            "microphone operation reply ABI");

        var pcmPayload = CreateMicPcmPayload(
            generation: 9, sequence: 17, captureTimeUs: 0x0102030405060708,
            frameCount: 2, Protocol.MicPcmFlags.StreamStart | Protocol.MicPcmFlags.Silence);
        var pcm = Protocol.DecodeMicPcm(pcmPayload);
        Require(pcm.Generation == 9 && pcm.Sequence == 17 &&
                pcm.CaptureTimeUs == 0x0102030405060708 && pcm.FrameCount == 2 &&
                pcm.Flags == (Protocol.MicPcmFlags.StreamStart | Protocol.MicPcmFlags.Silence) &&
                pcm.Pcm.Length == 4,
            "microphone PCM payload ABI");
        RequireInvalidData(() => Protocol.DecodeMicPcm(pcmPayload[..^1]),
            "microphone PCM exact length rejection");
        RequireInvalidData(() => Protocol.DecodeMicPcm(CreateMicPcmPayload(
                9, 18, 0, Protocol.MaxMicPcmFrames + 1, Protocol.MicPcmFlags.None)),
            "microphone PCM frame limit rejection");
        var unknownFlags = CreateMicPcmPayload(9, 18, 0, 1, Protocol.MicPcmFlags.None);
        BinaryPrimitives.WriteUInt16LittleEndian(unknownFlags.AsSpan(18, 2), 0x8000);
        RequireInvalidData(() => Protocol.DecodeMicPcm(unknownFlags),
            "microphone PCM unknown flags rejection");

        var status = Protocol.EncodeMicStatus(new Protocol.MicrophoneStatus(
            3, Protocol.MicrophoneState.RemoteActive, true, 1_920, 4, 5, 6, -7));
        Require(status.Length == Protocol.MicStatusPayloadSize &&
                BinaryPrimitives.ReadUInt32LittleEndian(status.AsSpan(0, 4)) == 3 &&
                status[4] == (byte)Protocol.MicrophoneState.RemoteActive && status[5] == 1 &&
                status[6] == 0 && status[7] == 0 &&
                BinaryPrimitives.ReadUInt32LittleEndian(status.AsSpan(8, 4)) == 1_920 &&
                BinaryPrimitives.ReadInt32LittleEndian(status.AsSpan(24, 4)) == -7,
            "microphone status ABI");
    }

    private static void VerifyMicrophonePcmQueue()
    {
        using var queue = new MicrophonePcmQueue(targetFrames: 480, maximumFrames: 1_440);
        queue.Reset(7);
        for (uint sequence = 0; sequence < 4; ++sequence)
        {
            var packet = Protocol.DecodeMicPcm(CreateMicPcmPayload(
                7, sequence, sequence * 10_000, 480, Protocol.MicPcmFlags.None));
            Require(queue.Enqueue(packet) == MicrophonePcmQueue.EnqueueResult.Accepted,
                "microphone PCM queue enqueue");
        }
        Require(queue.BufferedFrames == 480 && queue.DroppedFrames == 1_440,
            "microphone PCM queue bounded overflow");
        Require(queue.TryDequeue(out var overflowBlock) && overflowBlock is not null,
            "microphone PCM queue overflow survivor");
        var acceptedOverflowBlock = overflowBlock!;
        using (acceptedOverflowBlock)
        {
            Require(acceptedOverflowBlock.Sequence == 3 &&
                    acceptedOverflowBlock.Flags.HasFlag(Protocol.MicPcmFlags.Discontinuity),
                "microphone PCM overflow discontinuity");
        }

        queue.Reset(8);
        queue.Enqueue(Protocol.DecodeMicPcm(CreateMicPcmPayload(
            8, 10, 0, 480, Protocol.MicPcmFlags.None)));
        queue.Enqueue(Protocol.DecodeMicPcm(CreateMicPcmPayload(
            8, 12, 10_000, 480, Protocol.MicPcmFlags.None)));
        Require(queue.SequenceGaps == 1 && queue.BufferedFrames == 480,
            "microphone PCM sequence gap clears stale audio");
        Require(queue.TryDequeue(out var gapBlock) && gapBlock is not null,
            "microphone PCM sequence gap survivor");
        var acceptedGapBlock = gapBlock!;
        using (acceptedGapBlock)
        {
            Require(acceptedGapBlock.Sequence == 12 &&
                    acceptedGapBlock.Flags.HasFlag(Protocol.MicPcmFlags.Discontinuity),
                "microphone PCM sequence gap discontinuity");
        }

        var mismatch = Protocol.DecodeMicPcm(CreateMicPcmPayload(
            7, 13, 20_000, 480, Protocol.MicPcmFlags.None));
        Require(queue.Enqueue(mismatch) == MicrophonePcmQueue.EnqueueResult.GenerationMismatch &&
                queue.GenerationMismatches == 1 && queue.BufferedFrames == 0,
            "microphone PCM generation mismatch rejection");
        queue.Flush();
        Require(queue.BufferedBytes == 0, "microphone PCM flush");
    }

    private static void VerifyVirtualMicrophoneSession()
    {
        var input = new FakeMicrophoneInput(channels: 2);
        var messages = new List<Protocol.Message>();
        var deviceDisposed = false;
        using (var session = new VirtualMicrophoneSession(
                   input, () => deviceDisposed = true, generation: 21,
                   messages.Add, startPump: false))
        {
            session.PublishStatus();
            Require(messages[^1].Type == Protocol.MessageType.MicStatus,
                "virtual microphone initial status");

            var pcmPayload = CreateMicPcmPayload(
                21, 1, 10_000, 2, Protocol.MicPcmFlags.StreamStart);
            BinaryPrimitives.WriteInt16LittleEndian(pcmPayload.AsSpan(20, 2), 0x1234);
            BinaryPrimitives.WriteInt16LittleEndian(pcmPayload.AsSpan(22, 2), -2);
            session.Enqueue(Protocol.DecodeMicPcm(pcmPayload));
            var statusCountAfterStreamStart = messages.Count;
            session.Enqueue(Protocol.DecodeMicPcm(CreateMicPcmPayload(
                21, 2, 20_000, 0, Protocol.MicPcmFlags.Discontinuity)));
            Require(messages.Count == statusCountAfterStreamStart,
                "virtual microphone repeated PCM flags do not flood status queue");
            input.SetStreaming(true);
            session.PumpOnce();
            Require(input.Submissions.Count == 1 &&
                    input.Submissions[0].SequenceEqual(new byte[]
                    {
                        0x34, 0x12, 0x34, 0x12,
                        0xfe, 0xff, 0xfe, 0xff,
                    }),
                "virtual microphone mono to stereo submission");
            input.BufferedBytes = 1_920;
            var activeStatus = session.GetStatus();
            Require(activeStatus.State == Protocol.MicrophoneState.RemoteActive &&
                    activeStatus.IsHostStreaming && activeStatus.BufferedBytes == 1_920,
                "virtual microphone remote active state");
            input.SetStreaming(false);
            var stoppedStatus = session.GetStatus();
            Require(stoppedStatus.State == Protocol.MicrophoneState.RemoteActive &&
                    !stoppedStatus.IsHostStreaming && stoppedStatus.BufferedBytes == 0,
                "virtual microphone hides stale runtime buffer after capture pin close");
            input.BufferedBytes = 0;
            input.SetStreaming(true);

            session.Flush();
            input.Submissions.Clear();
            session.PumpOnce();
            Require(input.Submissions.Count == 1 && input.Submissions[0].Length == 1_920 &&
                    input.Submissions[0].AsSpan().IndexOfAnyExcept((byte)0) == -1 &&
                    session.GetStatus().Underruns == 1,
                "virtual microphone underrun silence");

            input.AcceptedBytes = 0;
            for (uint sequence = 2; sequence < 12; ++sequence)
            {
                session.Enqueue(Protocol.DecodeMicPcm(CreateMicPcmPayload(
                    21, sequence, sequence * 10_000, 1, Protocol.MicPcmFlags.None)));
                session.PumpOnce();
            }
            var faulted = session.GetStatus();
            Require(faulted.State == Protocol.MicrophoneState.DeviceFaulted &&
                    faulted.SubmitErrors == 10 && faulted.DroppedFrames >= 10,
                "virtual microphone submit failure isolation");
        }
        Require(deviceDisposed, "virtual microphone device disposal");

        try
        {
            using var invalid = new VirtualMicrophoneSession(
                new FakeMicrophoneInput(channels: 3), () => { }, generation: 1,
                _ => { }, startPump: false);
            Require(false, "virtual microphone format rejection");
        }
        catch (InvalidOperationException)
        {
            // Expected.
        }
    }

    private static void VerifyMicrophoneRegistryGating()
    {
        var messages = new List<Protocol.Message>();
        using var registry = new DeviceRegistry(messages.Add);
        const Protocol.Capability microphoneCapabilities =
            Protocol.Capability.VirtualMicrophone |
            Protocol.Capability.PersistentDeviceHost |
            Protocol.Capability.MicrophoneStatus;
        Require((registry.Capabilities & microphoneCapabilities) == 0,
            "virtual microphone prototype default-off capability gating");

        registry.CreateMicrophone(31, new byte[]
        {
            0x80, 0xbb, 0x00, 0x00,
            Protocol.MicrophoneChannels, Protocol.MicrophoneBitsPerSample, 0, 0,
        });
        Require(messages.Count == 1 && messages[0].Type == Protocol.MessageType.MicCreateReply &&
                messages[0].RequestId == 31 &&
                BinaryPrimitives.ReadInt32LittleEndian(messages[0].Payload.AsSpan(0, 4)) ==
                    (int)Protocol.MicrophoneResult.TransportUnavailable,
            "virtual microphone disabled create reply");

        registry.FlushMicrophone(32, ReadOnlySpan<byte>.Empty);
        registry.DestroyMicrophone(33, ReadOnlySpan<byte>.Empty);
        Require(messages.Count == 3 &&
                messages[1].Type == Protocol.MessageType.MicFlushReply &&
                messages[2].Type == Protocol.MessageType.MicDestroyReply &&
                BinaryPrimitives.ReadInt32LittleEndian(messages[1].Payload.AsSpan(0, 4)) ==
                    (int)Protocol.MicrophoneResult.DeviceNotCreated &&
                BinaryPrimitives.ReadInt32LittleEndian(messages[2].Payload.AsSpan(0, 4)) ==
                    (int)Protocol.MicrophoneResult.DeviceNotCreated,
            "virtual microphone absent lifecycle replies");
    }

    private static byte[] CreateMicPcmPayload(
        uint generation, uint sequence, ulong captureTimeUs, int frameCount,
        Protocol.MicPcmFlags flags)
    {
        var payload = new byte[Protocol.MicPcmHeaderSize + checked(frameCount * 2)];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), generation);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), sequence);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(8, 8), captureTimeUs);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(16, 2), checked((ushort)frameCount));
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(18, 2), (ushort)flags);
        return payload;
    }

    private static void RequireInvalidData(Action action, string operation)
    {
        try
        {
            action();
            Require(false, operation);
        }
        catch (InvalidDataException)
        {
            // Expected.
        }
    }

    private static void VerifySensorTimestampEncoding()
    {
        Require(ControllerSession.EncodeSensorTimestamp(0) == 0,
            "DualSense sensor timestamp origin");
        Require(ControllerSession.EncodeSensorTimestamp(1_000_000) == 3_000_000,
            "DualSense sensor timestamp tick rate");
        Require(ControllerSession.EncodeSensorTimestamp(0x55555556) == 2,
            "DualSense sensor timestamp rollover");
    }

    private static void VerifyProfileSelection()
    {
        Require(DeviceRegistry.SelectProfileId(
                0, Protocol.AttachFlags.None, true, true) == "dualsense-hidonly",
            "HID-only attach profile selection");
        Require(DeviceRegistry.SelectProfileId(
                1, Protocol.AttachFlags.GenshinCompatibilityIdentity, true, true) ==
                DualSenseHapticsAudio.GenshinCompatibilityProfileId,
            "Genshin compatibility attach profile selection");
        Require(DeviceRegistry.SelectProfileId(
                1, Protocol.AttachFlags.None, true, true) ==
                DualSenseHapticsAudio.CompositeProfileId,
            "standard composite attach profile selection");
        try
        {
            DeviceRegistry.SelectProfileId(
                0, Protocol.AttachFlags.GenshinCompatibilityIdentity, true, true);
            Require(false, "Genshin compatibility HID attach rejection");
        }
        catch (InvalidDataException)
        {
            // Expected.
        }
    }

    private static void VerifyControllerStateSubmissionPolicy()
    {
        var policy = new ControllerStateSubmissionPolicy();
        Require(!policy.ObserveInput(0, true), "idle controller state coalescing");
        Require(policy.ObserveInput(0, false), "analog activation boundary");
        Require(!policy.ObserveInput(0, false), "continuous analog state coalescing");
        Require(policy.ObserveInput(0, true), "analog neutral boundary");
        Require(policy.ObserveInput(0x1000, true), "button press boundary");
        Require(policy.ObserveInput(0, true), "button release boundary");
    }

    private static void VerifyDefaultAudioEndpointClassification()
    {
        Require(Enum.GetUnderlyingType(typeof(DefaultAudioEndpointGuard.AudioRole)) == typeof(int),
            "default audio role COM width");
        var virtualDualSense = new[]
        {
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"SWD\MMDEVAPI\{0.0.0.00000000}.fixture", Array.Empty<string>()),
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"USB\VID_054C&PID_0CE6&MI_00\fixture",
                new[] { @"USB\VID_054C&PID_0CE6&MI_00" }),
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"ROOT\USB\0000", new[] { @"ROOT\HIDMAESTRO_UDE" }),
        };
        Require(DefaultAudioEndpointGuard.IsVirtualDualSenseChain(virtualDualSense),
            "virtual HIDMaestro DualSense endpoint classification");

        Require(!DefaultAudioEndpointGuard.IsVirtualDualSenseChain(virtualDualSense[..2]),
            "physical DualSense endpoint exclusion");
        Require(!DefaultAudioEndpointGuard.IsVirtualDualSenseChain(new[]
        {
            new DefaultAudioEndpointGuard.DeviceNodeIdentity(
                @"ROOT\USB\0000", new[] { @"ROOT\HIDMAESTRO_UDE" }),
        }), "unrelated HIDMaestro endpoint exclusion");
    }

    private static void VerifyDefaultAudioEndpointPolicy()
    {
        Require(DefaultAudioEndpointPolicy.NeedsUpdate(null, null),
            "missing default audio endpoint policy");
        Require(DefaultAudioEndpointPolicy.NeedsUpdate(
                "{00000000-0000-0000-0000-000000000000}", 0x00000101),
            "partial default audio endpoint policy");
        Require(!DefaultAudioEndpointPolicy.NeedsUpdate(
                "{00000000-0000-0000-0000-000000000000}", 0x00000307),
            "complete default audio endpoint policy");
        var quadraphonic = DualSenseSpeakerConfiguration.CreateQuadraphonicFormat();
        Require(DualSenseSpeakerConfiguration.HasValidInteropLayout(),
            "Core Audio interop ABI");
        Require(DualSenseSpeakerConfiguration.IsQuadraphonic(quadraphonic),
            "quadraphonic Core Audio speaker configuration");
        quadraphonic.ChannelMask = 0x00000003;
        Require(!DualSenseSpeakerConfiguration.IsQuadraphonic(quadraphonic),
            "stereo Core Audio speaker configuration rejection");
    }

    private static void VerifyBundledCompositeProfile()
    {
        using var stream = typeof(ProtocolSelfTest).Assembly.GetManifestResourceStream(
            "Sunshine.Ds5Sidecar.profiles.dualsense-composite.json")
            ?? throw new InvalidOperationException("Bundled composite profile is missing");
        using var memory = new MemoryStream();
        stream.CopyTo(memory);
        var profileJson = DualSenseHapticsAudio.CreateRuntimeCompositeProfile(memory.ToArray());
        DualSenseHapticsAudio.ValidateCompositeProfile(profileJson);
        var compatibilityProfile = DualSenseHapticsAudio.CreateGenshinCompatibilityProfile(profileJson);
        using (var compatibilityDocument = JsonDocument.Parse(compatibilityProfile))
        {
            var root = compatibilityDocument.RootElement;
            Require(root.GetProperty("id").GetString() ==
                    DualSenseHapticsAudio.GenshinCompatibilityProfileId,
                "Genshin compatibility profile id");
            Require(root.GetProperty("productString").GetString() ==
                    DualSenseHapticsAudio.GenshinCompatibilityProductString,
                "Genshin compatibility product string");
            Require(root.GetProperty("vid").GetString() == "0x054C" &&
                    root.GetProperty("pid").GetString() == "0x0CE6",
                "Genshin compatibility profile preserves Sony identity");
        }

        var profileText = System.Text.Encoding.UTF8.GetString(profileJson);
        RequireProfileRejected(profileText.Replace("\"channels\":4", "\"channels\":2", StringComparison.Ordinal),
            "stereo composite profile rejection");
        RequireProfileRejected(profileText.Replace(
                "\"hapticLeft\",\"hapticRight\"",
                "\"hapticRight\",\"hapticLeft\"",
                StringComparison.Ordinal),
            "swapped haptics role rejection");
        RequireProfileRejected(profileText.Replace(
                "\"volumeCurRaw\":0",
                "\"volumeCurRaw\":-25600",
                StringComparison.Ordinal),
            "muted speaker control rejection");
    }

    private static void RequireProfileRejected(string profileJson, string operation)
    {
        try
        {
            DualSenseHapticsAudio.ValidateCompositeProfile(System.Text.Encoding.UTF8.GetBytes(profileJson));
            Require(false, operation);
        }
        catch (InvalidDataException)
        {
            // Expected.
        }
    }

    private static void VerifyHapticsChannelIsolation()
    {
        var frames = new byte[DualSenseHapticsAudio.InputFrameBytes * 2];
        WriteSample(frames, 0, 1234);
        WriteSample(frames, 1, -2345);
        WriteSample(frames, 4, short.MaxValue);
        WriteSample(frames, 5, short.MinValue);
        var speakerOnly = DualSenseHapticsAudio.Extract(frames);
        Require(speakerOnly.AsSpan().IndexOfAnyExcept((byte)0) == -1,
            "speaker channels cannot leak into haptics");

        WriteSample(frames, 2, 3456);
        WriteSample(frames, 3, -4567);
        WriteSample(frames, 6, 5678);
        WriteSample(frames, 7, -6789);
        var haptics = DualSenseHapticsAudio.Extract(frames);
        Require(BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(0, 2)) == 3456 &&
                BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(2, 2)) == -4567 &&
                BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(4, 2)) == 5678 &&
                BinaryPrimitives.ReadInt16LittleEndian(haptics.AsSpan(6, 2)) == -6789,
            "haptics channels preserve exact samples");

        try
        {
            DualSenseHapticsAudio.Extract(frames.AsSpan(0, frames.Length - 1));
            Require(false, "incomplete four-channel frame rejection");
        }
        catch (InvalidDataException)
        {
            // Expected: arbitrary callback fragmentation is reassembled by
            // ControllerSession before complete frames reach the extractor.
        }
    }

    private static void WriteSample(Span<byte> frames, int sample, short value) =>
        BinaryPrimitives.WriteInt16LittleEndian(frames.Slice(sample * 2, 2), value);

    private static async Task SendAsync(Stream stream, Protocol.Message message, CancellationToken cancellationToken)
    {
        var frame = Protocol.Encode(message);
        await stream.WriteAsync(frame, cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    private static async Task<Protocol.Message> ReceiveAsync(Stream stream, CancellationToken cancellationToken)
    {
        var headerBytes = new byte[Protocol.HeaderSize];
        await stream.ReadExactlyAsync(headerBytes, cancellationToken);
        var header = Protocol.DecodeHeader(headerBytes);
        var payload = new byte[header.PayloadLength];
        if (payload.Length != 0)
            await stream.ReadExactlyAsync(payload, cancellationToken);
        return new Protocol.Message(header.Type, header.RequestId, payload);
    }

    private static async Task<Protocol.Message> ReceiveUntilAsync(
        Stream stream, Protocol.MessageType type, uint requestId, CancellationToken cancellationToken,
        Action<Protocol.Message>? observed = null)
    {
        while (true)
        {
            var message = await ReceiveAsync(stream, cancellationToken);
            observed?.Invoke(message);
            if (message.Type == Protocol.MessageType.Error && message.RequestId == requestId)
                throw new InvalidOperationException(DecodeError(message.Payload));
            if (message.Type == type && message.RequestId == requestId)
                return message;
        }
    }

    private static async Task<Protocol.Message> ReceiveUntilTypeAsync(
        Stream stream, Protocol.MessageType type, CancellationToken cancellationToken)
    {
        while (true)
        {
            var message = await ReceiveAsync(stream, cancellationToken);
            if (message.Type == Protocol.MessageType.Error)
                throw new InvalidOperationException(DecodeError(message.Payload));
            if (message.Type == type)
                return message;
        }
    }

    private static string DecodeError(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 8) return "Malformed sidecar error";
        var length = Math.Min(BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4)), (uint)(payload.Length - 8));
        return System.Text.Encoding.UTF8.GetString(payload.Slice(8, (int)length));
    }

    private static void WriteFloat(Span<byte> destination, float value) =>
        BinaryPrimitives.WriteInt32LittleEndian(destination, BitConverter.SingleToInt32Bits(value));

    private static void VerifyAdaptiveTriggerEncoding()
    {
        var state = new AdaptiveTriggerState();
        var left = Enumerable.Range(0x20, AdaptiveTriggerState.EffectSize).Select(value => (byte)value).ToArray();
        var right = Enumerable.Range(0x40, AdaptiveTriggerState.EffectSize).Select(value => (byte)value).ToArray();

        Require(state.TryUpdate(new Dictionary<string, object> { ["leftTriggerEffect"] = left },
                                default, 3, 2, out var leftMessage),
            "left adaptive trigger update");
        Require(leftMessage.Type == Protocol.MessageType.AdaptiveTriggers &&
                leftMessage.Payload.Length == 26 &&
                leftMessage.Payload[0] == 3 && leftMessage.Payload[1] == 2 &&
                leftMessage.Payload[2] == AdaptiveTriggerState.LeftFlag &&
                leftMessage.Payload[3] == left[0] && leftMessage.Payload[4] == 0 &&
                leftMessage.Payload.AsSpan(6, 10).SequenceEqual(left.AsSpan(1, 10)),
            "left adaptive trigger encoding");

        Require(!state.TryUpdate(new Dictionary<string, object> { ["leftTriggerEffect"] = left },
                                 default, 3, 2, out _),
            "adaptive trigger duplicate suppression");

        Require(state.TryUpdate(new Dictionary<string, object> { ["rightTriggerEffect"] = right },
                                default, 3, 2, out var rightMessage) &&
                rightMessage.Payload[2] == AdaptiveTriggerState.RightFlag &&
                rightMessage.Payload[3] == left[0] && rightMessage.Payload[4] == right[0] &&
                rightMessage.Payload.AsSpan(16, 10).SequenceEqual(right.AsSpan(1, 10)),
            "right adaptive trigger encoding");

        Require(state.TryReset(3, 2, out var resetMessage) &&
                resetMessage.Payload[2] == (AdaptiveTriggerState.LeftFlag | AdaptiveTriggerState.RightFlag) &&
                resetMessage.Payload.AsSpan(3).IndexOfAnyExcept((byte)0) == -1,
            "adaptive trigger reset");
        Require(!state.TryReset(3, 2, out _), "adaptive trigger reset duplicate suppression");
    }

    private static void VerifyOutputValidityFlags()
    {
        var absent = OutputValidFlags.From(new Dictionary<string, object>());
        Require(absent.LeftTrigger && absent.RightTrigger && absent.Lightbar,
            "output validity fallback");

        var silent = Flags(0x00, 0x00);
        Require(!silent.LeftTrigger && !silent.RightTrigger && !silent.Lightbar,
            "output validity gates every field it governs");

        Require(Flags(0x0C, 0x00) is { LeftTrigger: true, RightTrigger: true },
            "output validity both triggers");
        Require(Flags(0x08, 0x00) is { LeftTrigger: true, RightTrigger: false },
            "output validity left trigger only");
        Require(Flags(0x04, 0x00) is { LeftTrigger: false, RightTrigger: true },
            "output validity right trigger only");
        Require(Flags(0x00, 0x44).Lightbar && !Flags(0x00, 0x40).Lightbar,
            "output validity lightbar control");

        // A byte the decoder hides governs nothing: exposing only the lightbar
        // byte must not start gating the triggers.
        var lightbarOnly = OutputValidFlags.From(new Dictionary<string, object>
        {
            ["validFlag1"] = (byte)0x40,
        });
        Require(!lightbarOnly.Lightbar &&
                lightbarOnly.LeftTrigger && lightbarOnly.RightTrigger,
            "output validity gates per byte");
    }

    private static void VerifyOutputValidityGating()
    {
        var effect = Enumerable.Range(0x22, AdaptiveTriggerState.EffectSize).Select(value => (byte)value).ToArray();
        var idle = new byte[AdaptiveTriggerState.EffectSize];
        var state = new AdaptiveTriggerState();

        // A report that programs one trigger leaves the other's bytes zero. The
        // trigger it is not programming keeps the effect the client holds. The
        // validFlag0 bits are spelled out so a wrong constant cannot make the
        // gate agree with itself.
        var rightOnly = new Dictionary<string, object>
        {
            ["leftTriggerEffect"] = idle,
            ["rightTriggerEffect"] = effect,
            ["validFlag0"] = (byte)0x04,
        };
        var leftOnly = new Dictionary<string, object>
        {
            ["leftTriggerEffect"] = effect,
            ["rightTriggerEffect"] = idle,
            ["validFlag0"] = (byte)0x08,
        };

        Require(state.TryUpdate(rightOnly, OutputValidFlags.From(rightOnly), 1, 0, out var right) &&
                right.Payload[2] == AdaptiveTriggerState.RightFlag &&
                right.Payload[4] == effect[0],
            "right trigger armed by its own report");
        Require(state.TryUpdate(leftOnly, OutputValidFlags.From(leftOnly), 1, 0, out var left) &&
                left.Payload[2] == AdaptiveTriggerState.LeftFlag &&
                left.Payload[3] == effect[0] && left.Payload[4] == effect[0],
            "left trigger armed without disturbing the right");
        Require(!state.TryUpdate(leftOnly, OutputValidFlags.From(leftOnly), 1, 0, out _),
            "an unprogrammed trigger is never released");

        // A report that says it is programming both triggers with no effect is
        // the game letting go, and it reaches the client as written.
        var releaseBoth = new Dictionary<string, object>
        {
            ["leftTriggerEffect"] = idle,
            ["rightTriggerEffect"] = idle,
            ["validFlag0"] = (byte)0x0C,
        };
        Require(state.TryUpdate(releaseBoth, OutputValidFlags.From(releaseBoth), 1, 0, out var release) &&
                release.Payload[2] == (AdaptiveTriggerState.LeftFlag | AdaptiveTriggerState.RightFlag) &&
                release.Payload.AsSpan(3).IndexOfAnyExcept((byte)0) == -1,
            "a programmed release reaches the client");
    }

    private static OutputValidFlags Flags(byte flag0, byte flag1) =>
        OutputValidFlags.From(new Dictionary<string, object>
        {
            ["validFlag0"] = flag0,
            ["validFlag1"] = flag1,
        });

    private static void Require(bool condition, string operation)
    {
        if (!condition) throw new InvalidOperationException($"Protocol self-test failed at {operation}");
    }

    private sealed class FakeMicrophoneInput : IVirtualMicrophoneInput
    {
        internal FakeMicrophoneInput(int channels)
        {
            Channels = channels;
        }

        public event Action<bool>? StreamingChanged;
        public int Channels { get; }
        public int SampleRateHz => 48_000;
        public int BitsPerSample => 16;
        public bool IsStreaming { get; private set; }
        public int BufferedBytes { get; set; }
        internal int? AcceptedBytes { get; set; }
        internal List<byte[]> Submissions { get; } = new();

        public int Submit(ReadOnlySpan<byte> pcm)
        {
            Submissions.Add(pcm.ToArray());
            return AcceptedBytes ?? pcm.Length;
        }

        internal void SetStreaming(bool streaming)
        {
            IsStreaming = streaming;
            StreamingChanged?.Invoke(streaming);
        }
    }
}
