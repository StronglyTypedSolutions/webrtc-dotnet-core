'use strict';

let retryHandle: number = NaN;

const canvas = document.getElementById("audio") as HTMLCanvasElement;
canvas.width = 512;
canvas.height = 256;

const canvasContext = canvas.getContext("2d");
const audioContext = new AudioContext();

function isPlaying(media: HTMLMediaElement): boolean {
    return media.currentTime > 0 && !media.paused && !media.ended && media.readyState > 2;
}

// https://github.com/webrtc/samples/blob/gh-pages/src/content/peerconnection/bandwidth/js/main.js
function removeBandwidthRestriction(sdp: string) {
    // TODO: This this is actually work? Test!
    return sdp.replace(/b=AS:.*\r\n/, '').replace(/b=TIAS:.*\r\n/, '');
}

const MediaRecorder = (window as any).MediaRecorder;

function createVolumeMeter(stream: MediaStream) {

    const context = audioContext;
    const track = context.createMediaStreamSource(stream);
    const gainNode = context.createGain();
    const analyser = context.createAnalyser();
    analyser.fftSize = 2048;
    track.connect(gainNode);
    track.connect(analyser);
    gainNode.gain.value = 0;
    track.connect(context.destination);
    const bufferLength = analyser.frequencyBinCount;
    console.log(bufferLength);
    const inputData = new Uint8Array(bufferLength);

    //var options = {
    //    audioBitsPerSecond: 22050,
    //    mimeType: 'audio/webm'
    //}

    //if (!MediaRecorder.isTypeSupported(options.mimeType)) alert(`${options.mimeType} is not supported`)!

    //const recorder = new MediaRecorder(stream, options);

    //recorder.ondataavailable = (packet: any) => {
    //    console.log("MediaRecorder", packet);
    //};
    //recorder.onerror = (err: any) => alert(`MediaRecorder error: ${err.message}`);
    //recorder.start();

    //mediaStreamSource.connect(audioContext.destination);
    //mediaStreamSource.connect(processor);
    //processor.connect(audioContext.destination);

    function drawAudio() {
        canvasContext.clearRect(0, 0, canvas.width, canvas.height);
        canvasContext.strokeStyle = "red";
        canvasContext.fillStyle = "red";

        canvasContext.lineWidth = 3;
        canvasContext.beginPath();

        analyser.getByteTimeDomainData(inputData);

        const inputDataLength = inputData.length;

        let total = 0;

        for (let i = 0; i < inputDataLength; i++) {
            const sample = inputData[i++];
            const y = sample * 128 + 128;
            if (i === 0) {
                canvasContext.moveTo(i, y);
            } else {
                canvasContext.lineTo(i, y);

            }
            total += Math.abs(sample);
        }

        canvasContext.stroke();

        const rms = Math.sqrt(total / inputDataLength);

        canvasContext.fillText(rms.toString(), 0, 20);

        requestAnimationFrame(drawAudio);
    };

    requestAnimationFrame(drawAudio);
}

function main() {

    retryHandle = NaN;

    const mediaStream = new MediaStream();
    const video = document.querySelector('video');
    video.srcObject = mediaStream ;

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

    video.addEventListener("readystatechange", () => log(`🛈 Video ready state = ${video.readyState}`));

    function sendMousePos(e: MouseEvent, kind: number) {
        const bounds = video.getBoundingClientRect();
        const x = (e.clientX - bounds.left) / bounds.width;
        const y = (e.clientY - bounds.top) / bounds.height;
        send("pos", { kind, x, y });

        if (kind === 2) {
            video.onmousemove = video.onmouseup = null;
        }
    }

    video.onmousedown = async (e: MouseEvent) => {
        if (e.button === 0) {
            sendMousePos(e, 0);
            video.onmousemove = (e2: MouseEvent) => sendMousePos(e2, 1);
            video.onmouseup = (e2: MouseEvent) => sendMousePos(e2, 2);
        }
    }

    playElem.onmousedown = async (e: MouseEvent) => {
        try {
            if (e.button === 0) {
                log(`🛈 Playing video`);
                await video.play();
                log(`🛈 Playing audio`);
                await audioContext.resume();
                log(`🛈 Player ready!`);
                playElem.style.visibility = "hidden";
            }
        } catch (err) {
            log(`✘ ${err}`);
        }
    }

    video.oncanplay = () => {
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

            if (track.kind === "video") {
               // video.srcObject = stream;
            } else {
                //video.srcObject = stream;
                createVolumeMeter(mediaStream);
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
