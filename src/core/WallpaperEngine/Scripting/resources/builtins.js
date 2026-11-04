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
  // an IAssetHandle: what createLayer takes (a record of the file)
  define('registerAsset', (file) => ({ file: String(file) }));
  // a layer is valid until a script destroys it (thisScene.destroyLayer)
  define('isObjectValid', (object) => object !== undefined && object !== null
    && (typeof object.__valid !== 'function' || object.__valid()));
})();

// console.warn/info/debug as V8 has them (log/error are native); scripts may also replace them (console.log = …)
(function () {
  const c = globalThis.console;
  if (c === undefined || c === null) {
    return;
  }
  c.warn = c.warn || c.error;
  c.info = c.info || c.log;
  c.debug = c.debug || c.log;
})();

// ILayer methods built on the natives (__world, __parentWorld, __setParent, __attachments) with Wallpaper Engine's own
// Mat4 (baseclasses.js; angles in degrees as SceneScript has them). `this` is the layer
globalThis.__layerMethods = (function () {
  const mat = (numbers) => {
    const m = new Mat4();
    for (let i = 0; i < 16; i++) {
      m.m[i] = numbers[i];
    }
    return m;
  };
  const attachment = (layer, which) => {
    const list = layer.__attachments();
    return typeof which === 'number' ? list[which] : list.find((a) => a.name === String(which));
  };
  // keep the layer where it is in the world while its parent changes: local = parent⁻¹ · world
  const adopt = (layer, world) => {
    const local = mat(layer.__parentWorld()).inverse().multiply(world).decompose();
    layer.origin = local.translation;
    layer.angles = local.rotation;
    layer.scale = local.scale;
  };
  // the rotation (degrees) whose −z axis points from `from` to `to`, `up` up (Mat4.lookAt is a view matrix: its
  // transpose turns the layer)
  const facing = (from, to, up) => mat(Mat4.lookAt(from, to, up || new Vec3(0, 1, 0)).m).transpose().extractEuler();
  return {
    // "the current world transform matrix"
    getTransformMatrix() {
      return mat(this.__world());
    },
    // "rotates the object around the axes defined by its current transformation"
    rotateObjectSpace(angles) {
      this.angles = Mat4.fromEuler(this.angles).multiply(Mat4.fromEuler(angles)).extractEuler();
    },
    lookAt(center, up) {
      const world = this.getTransformMatrix();
      this.angles = facing(world.translation(), center, up);
    },
    // as lookAt, turning about `up` only (heading): the target is moved level with the layer first
    lookAtYaw(center, up) {
      const u = (up || new Vec3(0, 1, 0)).normalize();
      const eye = this.getTransformMatrix().translation();
      const d = center.subtract(eye);
      const level = eye.add(d.subtract(u.multiply(d.dot(u))));
      this.angles = facing(eye, level, u);
    },
    // setParent (parent, adjustTransforms?) or setParent (parent, attachment, adjustTransforms?); an undefined parent
    // makes it a root layer
    setParent(parent, second, third) {
      const named = typeof second === 'string' || typeof second === 'number';
      const adjust = named ? third === true : second === true;
      let target = parent;
      if (typeof parent === 'string' || typeof parent === 'number') {
        target = typeof parent === 'number' ? thisScene.getLayer(parent) : thisScene.getLayer(String(parent));
      }
      let point;
      if (named && target !== undefined && target !== null) {
        const a = attachment(target, second);
        point = a === undefined ? undefined : a.name;
      }
      const world = adjust ? this.getTransformMatrix() : undefined;
      if (!this.__setParent(target === undefined ? null : target, point)) {
        return;
      }
      if (adjust) {
        adopt(this, world);
      }
    },
    getAttachmentIndex(name) {
      return this.__attachments().findIndex((a) => a.name === String(name));
    },
    // "world transform matrix of an attachment"
    getAttachmentMatrix(which) {
      const a = attachment(this, which);
      return a === undefined ? undefined : this.getTransformMatrix().multiply(mat(a.m));
    },
    getAttachmentOrigin(which) {
      const m = this.getAttachmentMatrix(which);
      return m === undefined ? undefined : m.translation();
    },
    getAttachmentAngles(which) {
      const m = this.getAttachmentMatrix(which);
      return m === undefined ? undefined : m.decompose().rotation;
    }
  };
})();

// IScene members over the natives
(function () {
  const scene = globalThis.thisScene;
  if (scene === undefined || scene === null) {
    return;
  }
  const define = (name, value) => Object.defineProperty(scene, name, { value, enumerable: true, configurable: true });
  define('getLayerByID', (id) => scene.enumerateLayers().find((layer) => String(layer.id) === String(id)));
})();

// V8's legacy RegExp statics (RegExp.$1…$9, lastMatch, …), which quickjs leaves out: scripts written for WE use them
// (2932157836's clock: `fmt.replace(RegExp.$1, …)` after a `test`). Every match (exec, test, match, replace, split go
// through `exec`) records the last one, as V8 does
(function () {
  if (Object.getOwnPropertyDescriptor(RegExp, '$1') !== undefined) {
    return;
  }
  const exec = RegExp.prototype.exec;
  let last = null;
  let input = '';
  Object.defineProperty(RegExp.prototype, 'exec', {
    value: function (s) {
      const result = exec.call(this, s);
      if (result !== null) {
        last = result;
        input = String(s);
      }
      return result;
    },
    writable: true,
    configurable: true
  });
  const getter = (name, get) => Object.defineProperty(RegExp, name, { get, configurable: true });
  for (let i = 1; i <= 9; i++) {
    getter('$' + i, () => (last !== null && last[i] !== undefined ? last[i] : ''));
  }
  const lastMatch = () => (last !== null ? last[0] : '');
  const lastParen = () => (last !== null && last.length > 1 && last[last.length - 1] !== undefined ? last[last.length - 1] : '');
  const left = () => (last !== null ? input.slice(0, last.index) : '');
  const right = () => (last !== null ? input.slice(last.index + last[0].length) : '');
  for (const [names, get] of [[['lastMatch', '$&'], lastMatch], [['lastParen', '$+'], lastParen],
    [['leftContext', '$`'], left], [['rightContext', "$'"], right], [['input', '$_'], () => input]]) {
    for (const name of names) {
      getter(name, get);
    }
  }
})();
