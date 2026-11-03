globalThis.__intervals = Object.create(null);
globalThis.localStorage = globalThis.localStorage || {
  __data: Object.create(null),
  get(key) {
    key = String(key);
    return Object.prototype.hasOwnProperty.call(this.__data, key) ? this.__data[key] : null;
  },
  set(key, value) { this.__data[String(key)] = String(value); },
  remove(key) { delete this.__data[String(key)]; },
  clear() { this.__data = Object.create(null); }
};
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
