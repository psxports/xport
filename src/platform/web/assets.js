(() => {
  const manifest = Module.xportManifest;
  const packages = new Map();
  const mounted = new Set();
  const database = new Promise(resolve => {
    const request = indexedDB.open('XPORT_ASSETS_' + manifest.name, 1);
    request.onupgradeneeded = () => request.result.createObjectStore('assets');
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => resolve(null);
    request.onblocked = () => resolve(null);
  }).catch(() => null);

  async function cached(key, asset) {
    const db = await database;
    if (!db) return null;
    return new Promise(resolve => {
      const request = db.transaction('assets').objectStore('assets').get(key);
      request.onsuccess = () => resolve(request.result?.hash === asset.hash ? request.result.data : null);
      request.onerror = () => resolve(null);
    }).catch(() => null);
  }

  async function save(key, asset, data) {
    const hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)), byte => byte.toString(16).padStart(2, '0')).join('');
    if (hash !== asset.hash) throw new Error('Asset checksum mismatch');
    const db = await database;
    if (!db) return;
    await new Promise(resolve => {
      const transaction = db.transaction('assets', 'readwrite');
      transaction.objectStore('assets').put({hash, data}, key);
      transaction.oncomplete = transaction.onerror = transaction.onabort = resolve;
    }).catch(() => {});
  }

  database.then(db => {
    if (!db) return;
    indexedDB.deleteDatabase('XPORT_PRELOAD_CACHE_' + manifest.name);
    const valid = new Set(manifest.packages.map((_, i) => 'data/' + i)
      .concat(Object.keys(manifest.music).map(track => 'music/' + track))
      .concat(Object.keys(manifest.video || {}).map(videoCacheKey)));
    const request = db.transaction('assets', 'readwrite').objectStore('assets').openCursor();
    request.onsuccess = () => {
      const cursor = request.result;
      if (!cursor) return;
      if (!valid.has(cursor.key)) cursor.delete();
      cursor.continue();
    };
  }).catch(() => {});

  const videoLoads = new Map();

  function videoCacheKey(source) {
    return 'video/' + source.replace(/\\/g, '/').replace(/^\/+/, '').toUpperCase();
  }

  async function checksum(data) {
    return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)),
      byte => byte.toString(16).padStart(2, '0')).join('');
  }

  Module.xportLoadVideo = function (source, asset) {
    const key = videoCacheKey(source);
    const identity = key + '/' + asset.hash;
    if (videoLoads.has(identity)) return videoLoads.get(identity);
    const listeners = new Set();
    const state = {data: new Uint8Array(asset.size), received: 0, complete: false, failed: false, error: null};
    function notifyOne(callback) {
      try { callback(state); } catch (error) { console.error(error); }
    }
    function notify() { for (const callback of listeners) notifyOne(callback); }
    state.subscribe = function (callback) {
      listeners.add(callback);
      Promise.resolve().then(() => { if (listeners.has(callback)) notifyOne(callback); });
      return () => listeners.delete(callback);
    };
    videoLoads.set(identity, state);
    state.done = (async () => {
      let reader;
      try {
        const chunks = asset.chunks || [{offset: 0, size: asset.size, hash: asset.hash}];
        let end = 0;
        for (const chunk of chunks) {
          if (!Number.isSafeInteger(chunk.offset) || !Number.isSafeInteger(chunk.size) ||
              chunk.offset !== end || chunk.size <= 0 || !/^[a-f0-9]{64}$/.test(chunk.hash))
            throw new Error('Invalid movie chunk index');
          end += chunk.size;
        }
        if (end !== asset.size) throw new Error('Incomplete movie chunk index');
        const hit = await cached(key, asset);
        if (hit && hit.byteLength === asset.size && await checksum(hit) === asset.hash) {
          state.data = new Uint8Array(hit);
          state.received = asset.size;
          state.complete = true;
          notify();
          return state;
        }
        const response = await fetch(asset.url);
        if (!response.ok) throw new Error('Movie HTTP ' + response.status);
        reader = response.body.getReader();
        let downloaded = 0, index = 0;
        for (;;) {
          const {value, done} = await reader.read();
          if (done) break;
          if (value.byteLength > asset.size - downloaded) throw new Error('Movie exceeds expected size');
          state.data.set(value, downloaded);
          downloaded += value.byteLength;
          while (index < chunks.length && downloaded >= chunks[index].offset + chunks[index].size) {
            const chunk = chunks[index++];
            if (await checksum(state.data.subarray(chunk.offset, chunk.offset + chunk.size)) !== chunk.hash)
              throw new Error('Movie chunk checksum mismatch');
            state.received = chunk.offset + chunk.size;
            notify();
          }
        }
        if (downloaded !== asset.size || state.received !== asset.size) throw new Error('Incomplete movie');
        await save(key, asset, state.data);
        state.complete = true;
      } catch (error) {
        state.failed = true;
        state.error = String(error && error.message || error);
        if (reader) { try { await reader.cancel(); } catch (_) {} }
      } finally {
        videoLoads.delete(identity);
        notify();
      }
      return state;
    })();
    return state;
  };

  async function download(asset, progress) {
    const response = await fetch(asset.url);
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const data = new Uint8Array(asset.size);
    let received = 0;
    const reader = response.body.getReader();
    for (;;) {
      const {value, done} = await reader.read();
      if (done) break;
      data.set(value, received);
      received += value.length;
      progress?.(received, asset.size);
    }
    if (received !== asset.size) throw new Error('Incomplete asset');
    return data;
  }

  function loadPackage(index, reportProgress = false) {
    if (!packages.has(index)) {
      packages.set(index, (async () => {
        const asset = manifest.packages[index];
        let data = await cached('data/' + index, asset);
        if (!data) {
          data = await download(asset, reportProgress ?
            (done, total) => Module.setStatus(`Downloading data... (${done}/${total})`) : null);
          await save('data/' + index, asset, data);
        }
        const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('gzip'));
        return new Uint8Array(await new Response(stream).arrayBuffer());
      })().catch(error => { packages.delete(index); throw error; }));
    }
    return packages.get(index);
  }

  async function mountFile(path) {
    if (mounted.has(path)) return;
    const entry = manifest.files[path];
    if (!entry) return;
    const data = new Uint8Array(entry.size);
    for (const [index, source, target, length] of entry.parts) {
      const bytes = await loadPackage(index);
      data.set(bytes.subarray(source, source + length), target);
    }
    Module.FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
    Module.FS.writeFile(path, data);
    mounted.add(path);
    for (const [index] of entry.parts) {
      if (Object.entries(manifest.files).every(([name, file]) => mounted.has(name) || file.parts.every(part => part[0] !== index)))
        packages.delete(index);
    }
  }

  async function withRetry(action) {
    const overlay = document.getElementById('loading');
    for (;;) {
      try { return await action(); }
      catch (error) {
        overlay.hidden = false;
        overlay.textContent = 'Download failed — click to retry';
        await new Promise(resolve => overlay.onclick = resolve);
        overlay.onclick = null;
      }
    }
  }

  Module.xportEnsureFile = async path => {
    const parts = [];
    for (const part of path.replaceAll('\\', '/').split('/')) {
      if (!part || part === '.') continue;
      if (part === '..') parts.pop();
      else parts.push(part);
    }
    path = '/' + parts.join('/');
    if (!manifest.files[path] || mounted.has(path)) return;
    const overlay = document.getElementById('loading');
    await withRetry(() => mountFile(path));
    overlay.hidden = true;
  };

  Module.preRun = Module.preRun || [];
  Module.preRun.push(() => {
    Module.addRunDependency('startup');
    withRetry(async () => {
      for (const index of manifest.startupPackages || [0]) await loadPackage(index, true);
      const startupFiles = manifest.startupFiles || Object.keys(manifest.files).filter(
        path => manifest.files[path].parts.every(part => part[0] === 0));
      for (const path of startupFiles) await mountFile(path);
    }).then(() => {
      document.getElementById('loading').hidden = true;
      Module.removeRunDependency('startup');
    });
  });

  Module.xportMusic = new Map();
  Module.xportMusicOpen = track => {
    const asset = manifest.music[track];
    if (!asset) return null;
    const state = {data: new Uint8Array(asset.size), received: 0, failed: false};
    (async () => {
      const existing = await cached('music/' + track, asset);
      if (existing) {
        state.data.set(existing);
        state.received = asset.size;
        return;
      }
      const response = await fetch(asset.url);
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const reader = response.body.getReader();
      for (;;) {
        const {done, value} = await reader.read();
        if (done) break;
        state.data.set(value, state.received);
        state.received += value.length;
      }
      if (state.received !== asset.size) throw new Error('Incomplete music');
      await save('music/' + track, asset, state.data);
    })().catch(error => { state.failed = true; console.error('Music download failed:', track, error); });
    return state;
  };
})();
