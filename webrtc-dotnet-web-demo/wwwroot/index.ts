'use strict';

let retryHandle: number = NaN;

const canvas = document.getElementById("audio") as HTMLCanvasElement;
canvas.width = 512;
canvas.height = 256;

const canvasContext = canvas.getContext("2d");
const audioContext = new AudioContext();
audioContext.suspend();

function isPlaying(media: HTMLMediaElement): boolean {
    return media.currentTime > 0 && !media.paused && !media.ended && media.readyState > 2;
}

// https://github.com/webrtc/samples/blob/gh-pages/src/content/peerconnection/bandwidth/js/main.js
function removeBandwidthRestriction(sdp: string) {
    // TODO: This this is actually work? Test!
    return sdp.replace(/b=AS:.*\r\n/, '').replace(/b=TIAS:.*\r\n/, '');
}

function main() {

    retryHandle = NaN;

    const mediaStream = new MediaStream();

    const videoElement = document.querySelector('video');
    videoElement.srcObject = mediaStream ;

    const logElem = document.getElementById('log');
    const playElem = document.getElementById('play-trigger');
    playElem.style.visibility = "hidden";

    // Clear log
    logElem.innerText = "";

    function log(text: string, ...extra: any[]) {

        console.log(text, ...extra);

        const line = document.createElement("pre");
        line.innerText = text;
        logElem.appendChild(line);
    }

    function getSignalingSocketUrl() {
        const scheme = location.protocol === "https:" ? "wss" : "ws";
        const port = location.port ? (":" + location.port) : "";
        const url = scheme + "://" + location.hostname + port + "/signaling";
        return url;
    }

    function startAudioSpectrumAnalyser() {
        const source = audioContext.createMediaStreamSource(mediaStream);

        const analyser = audioContext.createAnalyser();
        analyser.fftSize = canvas.width / 2;
        source.connect(analyser);

        // Make sure we don't play the audio twice by setting the volume to zero
        const gain = audioContext.createGain();
        gain.gain.value = 0;
        source.connect(gain);

        gain.connect(audioContext.destination);


        const bufferLength = analyser.frequencyBinCount;
        const inputData = new Uint8Array(bufferLength);

        function drawAudio() {
            canvasContext.clearRect(0, 0, canvas.width, canvas.height);
            canvasContext.fillStyle = "red";

            analyser.getByteTimeDomainData(inputData);

            const inputDataLength = inputData.length;

            const xScale = canvas.width / inputDataLength;
            const yScale = canvas.height / 256;

            for (let i = 0; i < inputDataLength; i++) {
                const sample = inputData[i];
                const height = sample * yScale;
                canvasContext.fillRect(i * xScale, canvas.height - height, xScale, height);
            }

            if (mediaStream.getAudioTracks().length) {
                requestAnimationFrame(drawAudio);
            } else {
                log("Stopped audio analyser");
            }
        };

        requestAnimationFrame(drawAudio);
    }

    const pc_config: RTCConfiguration = {
        iceServers: [
            { urls: "stun:stun.l.google.com:19302" }
        ]
    };

    // https://docs.google.com/document/d/1-ZfikoUtoJa9k-GZG1daN0BU3IjIanQ_JSscHxQesvU/edit#
    (pc_config as any)["sdpSemantics"] = "unified-plan";

    let pc = new RTCPeerConnection(pc_config);

    let ws = new WebSocket(getSignalingSocketUrl());
    ws.binaryType = "arraybuffer";

    function send(action: "ice" | "sdp" | "pos", payload: any) {
        const msg = JSON.stringify({ action, payload });
        log(`🛈 send ${msg}`);
        ws.send(msg);
    }

    function retry(reason: string) {
        clearTimeout(retryHandle);
        retryHandle = NaN;

        log(`✘ retrying in 1 second: ${reason}`);

        ws.close();
        pc.close();

        pc = null;
        ws = null;

        setTimeout(main, 1000);
    }

    videoElement.addEventListener("readystatechange", () => log(`🛈 Video ready state = ${videoElement.readyState}`));

    function sendMousePos(e: MouseEvent, kind: number) {
        const bounds = videoElement.getBoundingClientRect();
        const x = (e.clientX - bounds.left) / bounds.width;
        const y = (e.clientY - bounds.top) / bounds.height;
        send("pos", { kind, x, y });

        if (kind === 2) {
            videoElement.onmousemove = videoElement.onmouseup = null;
        }
    }

    videoElement.onmousedown = async (e: MouseEvent) => {
        if (e.button === 0) {
            sendMousePos(e, 0);
            videoElement.onmousemove = (e2: MouseEvent) => sendMousePos(e2, 1);
            videoElement.onmouseup = (e2: MouseEvent) => sendMousePos(e2, 2);
        }
    }

    playElem.onmousedown = async (e: MouseEvent) => {
        try {
            if (e.button === 0) {
                log(`🛈 Starting audio analyser...`);
                await audioContext.resume();
                log(`🛈 Playing video...`);
                await videoElement.play();
                log(`🛈 Player ready!`);
                playElem.style.visibility = "hidden";
            }
        } catch (err) {
            log(`✘ ${err}`);
        }
    }

    videoElement.oncanplay = () => {
        log(`🛈 Video can play`);
        playElem.style.visibility = "visible";
    };

    ws.onerror = () => log(`✘ websocket error`);
    ws.onclose = () => retry("websocket closed");

    ws.onopen = () => {
        pc.onicecandidate = e => {
            send("ice", e.candidate);
        };

        pc.onicegatheringstatechange = e => {
            log(`🛈 ice gathering state = ${pc && pc.iceGatheringState}`);
        };

        pc.oniceconnectionstatechange = e => {
            log(`🛈 ice connection state = ${pc && pc.iceConnectionState}`);
        };

        pc.onicecandidateerror = e => {
            log(`✘ ice candidate error = ${e.errorText}#${e.errorCode}`);
        };

        pc.ontrack = ({ streams, track }) => {

            log(`✔ received ${track.kind} track #${track.id} '${track.label}'`, track, streams);

            mediaStream.addTrack(track);

            if (track.kind === "audio") {
                startAudioSpectrumAnalyser();
            }

            track.onunmute = () => {
                log(`✔ ${track.kind} track #${track.id} '${track.label}' unmuted`, track);
            }

            track.onended = () => {
                mediaStream.removeTrack(track);
                log(`✘ ${track.kind} track #${track.id} '${track.label}' ended`, track);
            }

            track.onmute = () => {
                log(`✘ ${track.kind} track #${track.id} '${track.label}' muted`, track);
            };
        }
    }

    ws.onmessage = async e => {
        const { action, payload } = JSON.parse(e.data);
        log(`🛈 received ${e.data}`);

        try {
            switch (action) {
                case "ice":
                    {
                        await pc.addIceCandidate(payload);
                        log(`✔ addIceCandidate`);
                        break;
                    }

                case "sdp":
                    {
                        await pc.setRemoteDescription(payload);
                        log(`✔ setRemoteDescription`);
                        let { sdp, type } = await pc.createAnswer({ offerToReceiveVideo: true, offerToReceiveAudio: true });
                        log(`✔ createAnswer`);
                        sdp = removeBandwidthRestriction(sdp);
                        await pc.setLocalDescription({ sdp, type });
                        log(`✔ setLocalDescription`);
                        send("sdp", { sdp, type });
                    }
            }
        } catch (err) {
            log(`✘ ${err}`, err);
        }
    }
}

main();
