(function () {
  'use strict';
  let generation = 0;
  let session = null;
  const rate = 44100;
  const videoLimit = 4;
  const audioLimit = rate;

  function normalize(path) {
    path = path.replace(/\\/g, '/').replace(/;1$/i, '').toUpperCase();
    if (!path.startsWith('/')) path = '/' + path;
    if (!path.startsWith('/DATA/')) path = '/DATA' + path;
    return path;
  }

  function description(value) {
    return Uint8Array.from(atob(value), character => character.charCodeAt(0));
  }

  function live(state) {
    return session === state && state.generation === generation;
  }

  function fail(state, error) {
    if (!live(state)) return;
    state.error = String(error && error.message || error);
    console.error('Video: ' + state.error);
    if (state.reject) state.reject(new Error(state.error));
  }

  function audioEnd(state) {
    const tail = state.audio[state.audio.length - 1];
    return tail ? tail.start + tail.frames : state.clock;
  }

  function prime(state) {
    if (!live(state) || state.error) return;
    if (state.video.length && (!state.audioTrack || audioEnd(state) >= rate / 4 || state.audioDone)) {
      if (state.resolve) state.resolve();
      state.resolve = state.reject = null;
    }
  }

  function pump(state) {
    if (!live(state) || state.error || !state.bytes || state.pumping) return;
    state.pumping = true;
    try {
      for (const type of ['video', 'audio']) {
        const track = type === 'video' ? state.videoTrack : state.audioTrack;
        if (!track) continue;
        const decoder = state[type + 'Decoder'];
        let index = state[type + 'Index'];
        while (index < track.samples.length && decoder.decodeQueueSize < 4) {
          if (type === 'video' && state.video.length + index - state.videoDecoded >= videoLimit) break;
          if (type === 'audio' && index - state.audioDecoded >= 4) break;
          if (type === 'audio' && audioEnd(state) - state.clock >= audioLimit) break;
          const sample = track.samples[index];
          if (sample.offset + sample.size > state.received) break;
          ++index;
          const Chunk = type === 'video' ? EncodedVideoChunk : EncodedAudioChunk;
          decoder.decode(new Chunk({
            type: sample.key ? 'key' : 'delta', timestamp: sample.timestamp,
            duration: sample.duration, data: state.bytes.subarray(sample.offset, sample.offset + sample.size)
          }));
        }
        state[type + 'Index'] = index;
        if (index === track.samples.length && !state[type + 'Flushing']) {
          state[type + 'Flushing'] = true;
          decoder.flush().then(() => {
            if (!live(state)) return;
            if (type === 'audio' && state.pendingAudio) {
              appendAudio(state, state.pendingAudio, null);
              state.pendingAudio = null;
            }
            state[type + 'Done'] = true;
            prime(state);
          }).catch(error => fail(state, error));
        }
      }
    } catch (error) { fail(state, error); }
    finally { state.pumping = false; }
  }

  function videoOutput(state, frame) {
    try {
      if (!live(state) || state.error) return;
      const width = frame.displayWidth, height = frame.displayHeight;
      if (width !== state.entry.width || height !== state.entry.height || state.video.length >= videoLimit)
        throw new Error('Unexpected video geometry or full output queue');
      state.context.drawImage(frame, 0, 0, width, height);
      const rgba = state.context.getImageData(0, 0, width, height).data;
      const rgb = new Uint8Array(width * height * 3);
      for (let src = 0, dst = 0; dst < rgb.length; src += 4, dst += 3) {
        rgb[dst] = rgba[src]; rgb[dst + 1] = rgba[src + 1]; rgb[dst + 2] = rgba[src + 2];
      }
      state.video.push({rgb, timestamp: frame.timestamp + state.videoOffset, number: state.videoDecoded++});
      prime(state);
    } catch (error) { fail(state, error); }
    finally { frame.close(); }
    pump(state);
  }

  function appendAudio(state, data, nextData) {
      const planes = data.planes;
      // AAC priming has negative timestamps; discard its presentation before zero
      const start = Math.round((data.timestamp + state.audioOffset) * rate / 1000000);
      const endTime = Math.min(data.timestamp + data.numberOfFrames * 1000000 / data.sampleRate, state.audioTrack.duration);
      const end = Math.round((endTime + state.audioOffset) * rate / 1000000);
      const first = Math.max(start, Math.ceil(state.audioOffset * rate / 1000000));
      if (end > first) {
        const pcm = new Int16Array((end - first) * 2);
        for (let frame = first; frame < end; ++frame) {
          const position = (frame * 1000000 / rate - data.timestamp - state.audioOffset) * data.sampleRate / 1000000;
          const index = Math.floor(position);
          const fract = position - index;
          for (let channel = 0; channel < 2; ++channel) {
            const plane = planes[Math.min(channel, planes.length - 1)];
            const before = index < 0 ? state.audioLast ? state.audioLast[channel] : plane[0] : plane[Math.min(index, plane.length - 1)];
            const next = index + 1 >= plane.length ? nextData ? nextData.planes[Math.min(channel, nextData.planes.length - 1)][0] : plane[plane.length - 1] : plane[Math.max(0, index + 1)];
            const value = before + (next - before) * fract;
            pcm[(frame - first) * 2 + channel] = Math.max(-32768, Math.min(32767, Math.round(value * 32768)));
          }
        }
        state.audio.push({start: first, frames: end - first, pcm});
      }
      state.audioLast = [planes[0][planes[0].length - 1], planes[planes.length - 1][planes[0].length - 1]];
  }

  function audioOutput(state, data) {
    try {
      if (!live(state) || state.error) return;
      ++state.audioDecoded;
      if (data.numberOfChannels < 1 || data.numberOfChannels > 2) throw new Error('Unsupported audio channel count');
      const current = {timestamp: data.timestamp, numberOfFrames: data.numberOfFrames, sampleRate: data.sampleRate, planes: []};
      for (let channel = 0; channel < data.numberOfChannels; ++channel) {
        const plane = new Float32Array(data.numberOfFrames);
        data.copyTo(plane, {planeIndex: channel, format: 'f32-planar'});
        current.planes.push(plane);
      }
      // Keep one decoded packet so resampling can use the next packet's first sample
      if (state.pendingAudio) appendAudio(state, state.pendingAudio, current);
      state.pendingAudio = current;
      prime(state);
    } catch (error) { fail(state, error); }
    finally { data.close(); }
    pump(state);
  }

  Module.xportVideoClose = function () {
    ++generation;
    const old = session;
    session = null;
    if (!old) return;
    if (old.unsubscribe) old.unsubscribe();
    if (old.reject) old.reject(new Error('Video session closed'));
    for (const decoder of [old.videoDecoder, old.audioDecoder])
      if (decoder && decoder.state !== 'closed') decoder.close();
    old.video.length = old.audio.length = 0;
    old.pendingAudio = null;
    old.bytes = null;
  };

  Module.xportVideoOpen = async function (path) {
    Module.xportVideoClose();
    const source = normalize(path);
    const entries = Module.xportManifest && Module.xportManifest.video;
    const registered = entries && Object.keys(entries).find(key => normalize(key) === source);
    const entry = registered && entries[registered];
    if (!entry) throw new Error('Unregistered movie: ' + source);
    if (typeof VideoDecoder === 'undefined')
      throw new Error('WebCodecs is unavailable');
    const state = {
      generation, entry, source, bytes: null, received: 0,
      video: [], audio: [], videoIndex: 0, audioIndex: 0, videoDecoded: 0, audioDecoded: 0,
      videoDone: false, audioDone: false, clock: 0, underruns: 0, started: false, paused: false
    };
    session = state;
    try {
      state.videoTrack = entry.tracks.find(track => track.type === 'video');
      state.audioTrack = entry.tracks.find(track => track.type === 'audio');
      if (!state.videoTrack) throw new Error('Missing video track');
      const videoConfig = {codec: state.videoTrack.codec, description: description(state.videoTrack.description),
        codedWidth: entry.width, codedHeight: entry.height};
      if (!(await VideoDecoder.isConfigSupported(videoConfig)).supported) throw new Error('Unsupported video codec: ' + videoConfig.codec);
      let audioConfig;
      if (state.audioTrack) {
        if (typeof AudioDecoder === 'undefined') throw new Error('Audio WebCodecs is unavailable');
        audioConfig = {codec: state.audioTrack.codec, description: description(state.audioTrack.description),
          sampleRate: state.audioTrack.sampleRate, numberOfChannels: state.audioTrack.channels};
        if (!(await AudioDecoder.isConfigSupported(audioConfig)).supported) throw new Error('Unsupported audio codec: ' + audioConfig.codec);
      }
      if (!live(state)) throw new Error('Video session closed');
      const frames = entry.frames;
      if (!frames.length || !Number.isInteger(frames[0].endSector) || ![75, 150].includes(entry.sectorRate))
        throw new Error('Missing original CD completion timing');
      // Complete sectors, rather than MP4 decode speed, establish the original CD timeline
      state.videoOffset = (frames[0].endSector + 1) * 1000000 / entry.sectorRate;
      state.audioOffset = entry.sourceAudio && entry.sourceAudio.length ? entry.sourceAudio[0].first_sector * 1000000 / entry.sectorRate : 0;
      state.endTime = state.videoOffset + state.videoTrack.duration;
      if (state.audioTrack) state.endTime = Math.max(state.endTime, state.audioOffset + state.audioTrack.duration);
      state.canvas = new OffscreenCanvas(entry.width, entry.height);
      state.context = state.canvas.getContext('2d', {willReadFrequently: true});
      if (!state.context) throw new Error('Video RGB conversion unavailable');
      state.videoDecoder = new VideoDecoder({output: frame => videoOutput(state, frame), error: error => fail(state, error)});
      state.videoDecoder.configure(videoConfig);
      state.videoDecoder.addEventListener('dequeue', () => pump(state));
      if (audioConfig) {
        state.audioDecoder = new AudioDecoder({output: data => audioOutput(state, data), error: error => fail(state, error)});
        state.audioDecoder.configure(audioConfig);
        state.audioDecoder.addEventListener('dequeue', () => pump(state));
      } else state.audioDone = true;
      if (!Module.xportLoadVideo) throw new Error('Incremental movie transport is missing');
      const primed = new Promise((resolve, reject) => {state.resolve = resolve; state.reject = reject;});
      state.transport = Module.xportLoadVideo(source, entry);
      state.unsubscribe = state.transport.subscribe(transport => {
        if (!live(state)) return;
        if (transport.failed) {fail(state, transport.error); return;}
        state.bytes = transport.data;
        state.received = transport.received;
        pump(state);
      });
      await primed;
      if (!live(state)) throw new Error('Video session closed');
      state.startTime = performance.now();
      state.silentClock = 0;
      state.silentLastTime = state.startTime;
      state.started = true;
      return {width: entry.width, height: entry.height, received: state.received, size: entry.size, complete: state.transport.complete};
    } catch (error) {
      if (live(state)) { fail(state, error); Module.xportVideoClose(); }
      throw error;
    }
  };

  Module.xportVideoPause = function (paused) {
    const state = session;
    if (!state || state.paused === paused) return;
    const now = performance.now();
    if (state.started && !state.paused && !state.audioTrack && (state.video.length || state.videoDone))
      state.silentClock += (now - state.silentLastTime) * 1000;
    state.silentLastTime = now;
    state.paused = paused;
  };

  Module.xportVideoReadPcm = function (output, count) {
    const state = session;
    if (!state || !state.started || state.error || state.paused || !state.audioTrack) return 0;
    let filled = 0;
    while (filled < count) {
      const nextVideo = state.videoTrack.samples[state.videoDecoded];
      if (!state.video.length && !state.videoDone && nextVideo &&
          state.clock * 1000000 / rate >= nextVideo.timestamp + state.videoOffset) {
        ++state.underruns;
        break;
      }
      const block = state.audio[0];
      if (block && state.clock >= block.start + block.frames) {state.audio.shift(); continue;}
      if (!block && !state.audioDone) {++state.underruns; break;}
      if (!block || state.clock < block.start) {
        if (state.audioDone && state.clock * 1000000 / rate >= state.endTime) break;
        output[filled * 2] = output[filled * 2 + 1] = 0;
      } else {
        const offset = (state.clock - block.start) * 2;
        output[filled * 2] = block.pcm[offset]; output[filled * 2 + 1] = block.pcm[offset + 1];
      }
      ++state.clock; ++filled;
    }
    pump(state);
    return filled;
  };

  Module.xportVideoNext = function (output) {
    const state = session;
    if (!state || state.error) return {status: -1};
    if (state.paused) return {status: 0};
    if (!state.audioTrack) {
      const now = performance.now();
      if (state.video.length || state.videoDone) state.silentClock += (now - state.silentLastTime) * 1000;
      state.silentLastTime = now;
    }
    const time = state.audioTrack ? state.clock * 1000000 / rate : state.silentClock;
    const frame = state.video[0];
    if (frame && frame.timestamp <= time) {
      if (output.length < frame.rgb.length) {fail(state, 'Video RGB buffer too small'); return {status: -1};}
      output.set(frame.rgb);
      state.video.shift(); pump(state);
      return {status: 1, width: state.entry.width, height: state.entry.height, number: state.entry.frames[frame.number].number};
    }
    if (state.videoDone && !state.video.length && time >= state.endTime) return {status: 2};
    pump(state);
    return {status: 0};
  };
})();
