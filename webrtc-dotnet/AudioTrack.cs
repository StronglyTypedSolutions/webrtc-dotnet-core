using System;

namespace WonderMediaProductions.WebRtc
{
    /// <summary>
    /// TODO: Current an audio track must be created before a connection is established!
    /// </summary>
    public class AudioTrack : Disposable
    {
        public int TrackId { get; }

        public PeerConnection PeerConnection { get; }


        public AudioTrack(PeerConnection peerConnection, AudioEncoderOptions options)
        {
            PeerConnection = peerConnection;
            TrackId = peerConnection.AddAudioTrack(options);
        }

        protected override void OnDispose(bool isDisposing)
        {
            // TODO: Remove audio track!
        }
    }
}
