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
  // IAnimationLayer: one puppet animation layer of an image layer (natives __animLayers / __animLayerSet)
  const ended = [];
  class IAnimationLayer {
    constructor(layer, key) {
      Object.defineProperty(this, '__layer', { value: layer });
      Object.defineProperty(this, '__key', { value: key });
    }
    get __info() { return this.__layer.__animLayers().find((a) => a.key === this.__key); }
    get fps() { const i = this.__info; return i ? i.fps : 0; }
    get frameCount() { const i = this.__info; return i ? i.frames : 0; }
    get duration() { const i = this.__info; return i && i.fps > 0 ? i.frames / i.fps : 0; }
    get name() { const i = this.__info; return i ? i.name : ''; }
    set name(v) { this.__layer.__animLayerSet(this.__key, 'name', String(v)); }
    get rate() { const i = this.__info; return i ? i.rate : 0; }
    set rate(v) { this.__layer.__animLayerSet(this.__key, 'rate', +v); }
    get blend() { const i = this.__info; return i ? i.blend : 0; }
    set blend(v) { this.__layer.__animLayerSet(this.__key, 'blend', +v); }
    get visible() { const i = this.__info; return i ? i.visible : false; }
    set visible(v) { this.__layer.__animLayerSet(this.__key, 'visible', !!v); }
    play() { this.__layer.__animLayerSet(this.__key, 'playing', true); }
    pause() { this.__layer.__animLayerSet(this.__key, 'playing', false); }
    // "stop the animation and reset the current time to zero"
    stop() { this.__layer.__animLayerSet(this.__key, 'stop'); }
    isPlaying() { const i = this.__info; return i ? i.playing : false; }
    getFrame() { const i = this.__info; return i ? Math.floor(i.time * i.fps + 1e-4) : 0; }
    setFrame(frame) { const i = this.__info; if (i && i.fps > 0) { this.__layer.__animLayerSet(this.__key, 'time', frame / i.fps); } }
    // "fired every time the animation layer reaches the end"
    addEndedCallback(callback) {
      if (typeof callback !== 'function') { return; }
      const i = this.__info;
      ended.push({ layer: this, callback, seen: i ? i.ends : 0 });
    }
  }
  globalThis.IAnimationLayer = IAnimationLayer;
  // thisObject of a script on an animation layer's value (ScriptEngine::evaluateModule)
  globalThis.__animationLayerOf = (layer, key) => animationLayer(layer, key);
  const animationLayer = (layer, key) => {
    let cache = layer.__animationLayers;
    if (cache === undefined) {
      cache = new Map();
      layer.__animationLayers = cache;
    }
    if (!cache.has(key)) { cache.set(key, new IAnimationLayer(layer, key)); }
    return cache.get(key);
  };
  // createAnimationLayer's config: rate, blend, visible, name (WE: "see IAnimationLayer for config options")
  const configure = (anim, config) => {
    if (config === null || typeof config !== 'object') { return; }
    for (const key of ['rate', 'blend', 'visible', 'name']) {
      if (config[key] !== undefined) { anim[key] = config[key]; }
    }
    if (config.frame !== undefined) { anim.setFrame(config.frame); }
    if (config.paused === true || config.startpaused === true) { anim.pause(); }
  };
  const create = (layer, animation, config, single) => {
    let which = animation;
    if (animation !== null && typeof animation === 'object') {
      config = config || animation;
      which = animation.animation !== undefined ? animation.animation : animation.name;
    }
    const key = layer.__animLayerCreate(typeof which === 'number' ? which : String(which), single);
    if (key < 0) { return undefined; }
    const anim = animationLayer(layer, key);
    configure(anim, config);
    return anim;
  };
  const bone = (layer, which) => {
    const bones = layer.__bones();
    const index = typeof which === 'number' ? which : bones.findIndex((b) => b.name === String(which));
    return index >= 0 && index < bones.length ? { index, bones, bone: bones[index] } : undefined;
  };
  // runs once a frame (ScriptEngine::tick): ended callbacks of the layers that reached their end since
  globalThis.__tickHooks = function () {
    for (const entry of ended) {
      const i = entry.layer.__info;
      if (i === undefined || i.ends === entry.seen) { continue; }
      entry.seen = i.ends;
      try { entry.callback(); } catch (e) { console.error('addEndedCallback: ' + e); }
    }
  };
  return {
    getAnimationLayerCount() {
      return this.__animLayers().length;
    },
    // "an existing animation layer object by name or index"
    getAnimationLayer(which) {
      const list = this.__animLayers();
      const info = typeof which === 'number' ? list[which] : list.find((a) => a.name === String(which));
      return info === undefined ? undefined : animationLayer(this, info.key);
    },
    createAnimationLayer(animation, config) {
      return create(this, animation, config, false);
    },
    // "only play the animation once, then remove it automatically"
    playSingleAnimation(animation, config) {
      return create(this, animation, config, true);
    },
    destroyAnimationLayer(which) {
      const anim = which instanceof IAnimationLayer ? which : this.getAnimationLayer(which);
      return anim !== undefined && this.__animLayerDestroy(anim.__key);
    },
    getBoneCount() {
      return this.__bones().length;
    },
    getBoneIndex(name) {
      return this.__bones().findIndex((b) => b.name === String(name));
    },
    getBoneParentIndex(child) {
      const b = bone(this, child);
      return b === undefined ? -1 : b.bone.parent;
    },
    // a bone in the world: the layer's world · the bone's (the mesh's space is the layer's own)
    getBoneTransform(which) {
      const b = bone(this, which);
      return b === undefined ? undefined : this.getTransformMatrix().multiply(mat(b.bone.world));
    },
    setBoneTransform(which, transform) {
      const b = bone(this, which);
      if (b === undefined) { return; }
      const parent = b.bone.parent >= 0 ? mat(b.bones[b.bone.parent].world) : new Mat4();
      this.__boneOverride(b.index, this.getTransformMatrix().multiply(parent).inverse().multiply(transform).m);
    },
    getLocalBoneTransform(which) {
      const b = bone(this, which);
      return b === undefined ? undefined : mat(b.bone.local);
    },
    setLocalBoneTransform(which, transform) {
      const b = bone(this, which);
      if (b !== undefined) { this.__boneOverride(b.index, transform.m); }
    },
    getLocalBoneAngles(which) {
      const m = this.getLocalBoneTransform(which);
      return m === undefined ? undefined : m.decompose().rotation;
    },
    setLocalBoneAngles(which, angles) {
      const m = this.getLocalBoneTransform(which);
      if (m === undefined) { return; }
      const d = m.decompose();
      this.setLocalBoneTransform(which, Mat4.compose(d.translation, angles, d.scale));
    },
    getLocalBoneOrigin(which) {
      const m = this.getLocalBoneTransform(which);
      return m === undefined ? undefined : m.translation();
    },
    setLocalBoneOrigin(which, origin) {
      const m = this.getLocalBoneTransform(which);
      if (m === undefined) { return; }
      const d = m.decompose();
      this.setLocalBoneTransform(which, Mat4.compose(origin, d.rotation, d.scale));
    },
    // LWE simulates no bone physics and has no blend shapes (no library scene has either)
    applyBonePhysicsImpulse() {},
    resetBonePhysicsSimulation() {},
    getBlendShapeIndex() { return -1; },
    getBlendShapeWeight() { return 0; },
    setBlendShapeWeight() {},
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
  // CameraTransforms { eye, center, up, zoom }: the scene's camera (a 3D scene's view, any scene's zoom)
  class CameraTransforms {
    constructor(eye, center, up, zoom) { this.eye = eye; this.center = center; this.up = up; this.zoom = zoom; }
  }
  globalThis.CameraTransforms = CameraTransforms;
  define('getCameraTransforms', () => new CameraTransforms(...scene.__cameraTransforms()));
  define('setCameraTransforms', (t) => {
    if (t !== null && typeof t === 'object') { scene.__setCameraTransforms(t.eye, t.center, t.up, t.zoom); }
  });
  // getInitialLayerConfig (layer): its object as the scene file has it (a copy)
  let initial;
  define('getInitialLayerConfig', (which) => {
    const layer = typeof which === 'object' ? which : scene.getLayer(which);
    if (layer === undefined || layer === null) { return undefined; }
    if (initial === undefined) {
      try { initial = JSON.parse(scene.__sceneJson() || '{}'); } catch (e) { initial = {}; }
    }
    const found = (initial.objects || []).find((o) => o && o.id === layer.id);
    return found === undefined ? undefined : JSON.parse(JSON.stringify(found));
  });
  // createModelData / destroyModelData: LWE builds no meshes from script data (no library scene does)
  define('createModelData', () => undefined);
  define('destroyModelData', () => {});
})();

// renderContext: an empty interface in WE's definitions
globalThis.renderContext = globalThis.renderContext || {};

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

// IParticleSystemInstance: a particle layer's instance overrides, live (natives __particleGet / __particleSet)
(function () {
  class IParticleSystemInstance {
    constructor(layer) { Object.defineProperty(this, '__layer', { value: layer }); }
  }
  const names = ['alpha', 'size', 'count', 'speed', 'lifetime', 'rate', 'colorn'];
  for (let i = 0; i < 8; i++) { names.push('controlpoint' + i); }
  for (const name of names) {
    Object.defineProperty(IParticleSystemInstance.prototype, name, {
      get() { return this.__layer.__particleGet(name); },
      set(v) { this.__layer.__particleSet(name, v); },
      enumerable: true
    });
  }
  globalThis.IParticleSystemInstance = IParticleSystemInstance;
  const instances = new WeakMap();
  globalThis.__particleInstanceOf = (layer) => {
    if (!instances.has(layer)) { instances.set(layer, new IParticleSystemInstance(layer)); }
    return instances.get(layer);
  };
})();
