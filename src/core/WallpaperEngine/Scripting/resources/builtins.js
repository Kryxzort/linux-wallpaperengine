globalThis.__intervals = Object.create(null);
// SceneScript's ILocalStorage: get/set/delete/clear (key, location), LOCATION_GLOBAL ("global") and LOCATION_SCREEN
// ("screen", the default). Values keep their type (JSON), and persist in the project's file (__storage, the host's
// storage dir) up to WE's 100 KB; without one they live as long as the scene. One "screen" store per project (the core
// plays a project on one screen). A missing key reads undefined (WE's result is not documented); a stored vector comes
// back as a Vec2/3/4
(function () {
  const native = globalThis.__storage;
  let data = { global: {}, screen: {} };
  try {
    const text = native ? native.read() : '';
    if (text) {
      const stored = JSON.parse(text);
      if (stored && typeof stored === 'object') {
        data.global = stored.global && typeof stored.global === 'object' ? stored.global : {};
        data.screen = stored.screen && typeof stored.screen === 'object' ? stored.screen : {};
      }
    }
  } catch (e) {
    console.error('localStorage: the stored file is not JSON, starting empty');
  }
  let dirty = false;
  const store = (location) => (location === 'global' ? data.global : data.screen);
  const has = (bucket, key) => Object.prototype.hasOwnProperty.call(bucket, key);
  // a stored Vec2/Vec3/Vec4 comes back as one (JSON keeps only its x y z w)
  const revive = (key, value) => {
    if (value === null || typeof value !== 'object' || Array.isArray(value)) {
      return value;
    }
    const keys = Object.keys(value).sort().join('');
    const ctor = { xy: 'Vec2', xyz: 'Vec3', wxyz: 'Vec4' }[keys];
    if (ctor === undefined || !Object.values(value).every((v) => typeof v === 'number')) {
      return value;
    }
    try {
      const make = (0, eval)(ctor);
      return ctor === 'Vec2' ? new make(value.x, value.y)
        : ctor === 'Vec3' ? new make(value.x, value.y, value.z) : new make(value.x, value.y, value.z, value.w);
    } catch (e) {
      return value;
    }
  };
  globalThis.localStorage = {
    LOCATION_GLOBAL: 'global',
    LOCATION_SCREEN: 'screen',
    get(key, location) {
      const bucket = store(location);
      key = String(key);
      return has(bucket, key) ? JSON.parse(JSON.stringify(bucket[key]), revive) : undefined;
    },
    set(key, value, location) {
      store(location)[String(key)] = value === undefined ? null : JSON.parse(JSON.stringify(value));
      dirty = true;
    },
    delete(key, location) {
      const bucket = store(location);
      key = String(key);
      if (!has(bucket, key)) {
        return false;
      }
      delete bucket[key];
      dirty = true;
      return true;
    },
    // LWE's old name for delete
    remove(key, location) { return this.delete(key, location); },
    clear(location) {
      if (location === 'global') {
        data.global = {};
      } else {
        data.screen = {};
      }
      dirty = true;
    }
  };
  globalThis.__storageFlush = function () {
    if (!dirty) {
      return;
    }
    dirty = false;
    const text = JSON.stringify(data);
    if (text.length > 100 * 1024) {
      console.error('localStorage: over 100 KB (' + text.length + ' bytes), not saved');
      return;
    }
    if (native) {
      native.write(text);
    }
  };
})();
globalThis.MediaPlaybackEvent = globalThis.MediaPlaybackEvent || {
  PLAYBACK_STOPPED: 0,
  PLAYBACK_PLAYING: 1,
  PLAYBACK_PAUSED: 2
};
// A property script's createScriptProperties(): Wallpaper Engine's own builder (baseclasses.js) gives every key its
// default (a combo: its first option); the values the scene stores for the module being evaluated (live, bound to
// user properties: the native __storedScriptProperties) win over those, as WE's _Internal.updateScriptProperties
// does (a string for a vector default becomes that vector)
(function () {
  const stored = globalThis.__storedScriptProperties;
  const builder = globalThis.createScriptProperties;
  if (typeof builder !== 'function') {
    globalThis.createScriptProperties = stored;
    return;
  }
  globalThis.createScriptProperties = function () {
    const creator = builder();
    const finish = creator.finish;
    creator.finish = function () {
      const defaults = finish.call(creator);
      const values = stored().finish();
      if (values === undefined || values === null) {
        return defaults;
      }
      return new Proxy(defaults, {
        get(target, key, receiver) {
          const fallback = Reflect.get(target, key, receiver);
          if (typeof key !== 'string') {
            return fallback;
          }
          const value = values[key];
          if (value === undefined) {
            return fallback;
          }
          if (typeof value === 'string' && fallback !== null && typeof fallback === 'object'
              && typeof fallback.constructor === 'function' && /^Vec[234]$/.test(fallback.constructor.name)) {
            return new fallback.constructor(value);
          }
          return value;
        }
      });
    };
    return creator;
  };
})();

// IEngine members that are constants or plain JS here (WE's PC app, running as a wallpaper, outside its editor)
(function () {
  const engine = globalThis.engine;
  if (engine === undefined || engine === null) {
    return;
  }
  const define = (name, value) => Object.defineProperty(engine, name, { value, enumerable: true, configurable: true });
  define('isDesktopDevice', () => true);
  define('isMobileDevice', () => false);
  define('isWallpaper', () => true);
  define('isScreensaver', () => false);
  define('isRunningInEditor', () => false);
  define('isPortrait', () => { const r = engine.screenResolution; return r.y > r.x; });
  define('isLandscape', () => { const r = engine.screenResolution; return r.x >= r.y; });
  // an IAssetHandle: what createLayer would take (createLayer isn't supported, so a plain record of the file)
  define('registerAsset', (file) => ({ file: String(file) }));
  define('isObjectValid', (object) => object !== undefined && object !== null);
})();
