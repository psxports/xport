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
    const valid = new Set(manifest.packages.map((_, i) => 'data/' + i).concat(Object.keys(manifest.music).map(track => 'music/' + track)));
    const request = db.transaction('assets', 'readwrite').objectStore('assets').openCursor();
    request.onsuccess = () => {
      const cursor = request.result;
      if (!cursor) return;
      if (!valid.has(cursor.key)) cursor.delete();
      cursor.continue();
    };
  }).catch(() => {});

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
      await loadPackage(0, true);
      for (const [path, entry] of Object.entries(manifest.files))
        if (entry.parts.every(part => part[0] === 0)) await mountFile(path);
    }).then(() => {
      document.getElementById('loading').hidden = true;
      Module.removeRunDependency('startup');
    });
  });

  Module.xportMusic = new Map();
  Module.xportMusicOpen = async track => {
    const asset = manifest.music[track];
    if (!asset) return null;
    const state = {data: new Uint8Array(asset.size), received: 0, failed: false};
    let notify;
    const ready = new Promise(resolve => notify = resolve);
    (async () => {
      const existing = await cached('music/' + track, asset);
      if (existing) {
        state.data.set(existing);
        state.received = asset.size;
        notify(state);
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
        if (state.received >= Math.min(asset.size, 128 * 1024)) notify(state);
      }
      if (state.received !== asset.size) throw new Error('Incomplete music');
      await save('music/' + track, asset, state.data);
      notify(state);
    })().catch(() => { state.failed = true; notify(null); });
    return ready;
  };
})();
