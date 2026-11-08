// peer.ts
import fs from "node:fs";

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/errors.js
var HpkeError = class extends Error {
  constructor(e) {
    let message;
    if (e instanceof Error) {
      message = e.message;
    } else if (typeof e === "string") {
      message = e;
    } else {
      message = "";
    }
    super(message);
    this.name = this.constructor.name;
  }
};
var InvalidParamError = class extends HpkeError {
};
var SerializeError = class extends HpkeError {
};
var DeserializeError = class extends HpkeError {
};
var EncapError = class extends HpkeError {
};
var DecapError = class extends HpkeError {
};
var ExportError = class extends HpkeError {
};
var SealError = class extends HpkeError {
};
var OpenError = class extends HpkeError {
};
var MessageLimitReachedError = class extends HpkeError {
};
var DeriveKeyPairError = class extends HpkeError {
};
var NotSupportedError = class extends HpkeError {
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/_dnt.shims.js
var dntGlobals = {};
var dntGlobalThis = createMergeProxy(globalThis, dntGlobals);
function createMergeProxy(baseObj, extObj) {
  return new Proxy(baseObj, {
    get(_target, prop, _receiver) {
      if (prop in extObj) {
        return extObj[prop];
      } else {
        return baseObj[prop];
      }
    },
    set(_target, prop, value) {
      if (prop in extObj) {
        delete extObj[prop];
      }
      baseObj[prop] = value;
      return true;
    },
    deleteProperty(_target, prop) {
      let success3 = false;
      if (prop in extObj) {
        delete extObj[prop];
        success3 = true;
      }
      if (prop in baseObj) {
        delete baseObj[prop];
        success3 = true;
      }
      return success3;
    },
    ownKeys(_target) {
      const baseKeys = Reflect.ownKeys(baseObj);
      const extKeys = Reflect.ownKeys(extObj);
      const extKeysSet = new Set(extKeys);
      return [...baseKeys.filter((k) => !extKeysSet.has(k)), ...extKeys];
    },
    defineProperty(_target, prop, desc) {
      if (prop in extObj) {
        delete extObj[prop];
      }
      Reflect.defineProperty(baseObj, prop, desc);
      return true;
    },
    getOwnPropertyDescriptor(_target, prop) {
      if (prop in extObj) {
        return Reflect.getOwnPropertyDescriptor(extObj, prop);
      } else {
        return Reflect.getOwnPropertyDescriptor(baseObj, prop);
      }
    },
    has(_target, prop) {
      return prop in extObj || prop in baseObj;
    }
  });
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/algorithm.js
async function loadSubtleCrypto() {
  if (dntGlobalThis !== void 0 && globalThis.crypto !== void 0) {
    return globalThis.crypto.subtle;
  }
  try {
    const { webcrypto } = await import("crypto");
    return webcrypto.subtle;
  } catch (e) {
    throw new NotSupportedError(e);
  }
}
var NativeAlgorithm = class {
  constructor() {
    Object.defineProperty(this, "_api", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
  }
  async _setup() {
    if (this._api !== void 0) {
      return;
    }
    this._api = await loadSubtleCrypto();
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/identifiers.js
var Mode = {
  Base: 0,
  Psk: 1,
  Auth: 2,
  AuthPsk: 3
};
var KemId = {
  NotAssigned: 0,
  DhkemP256HkdfSha256: 16,
  DhkemP384HkdfSha384: 17,
  DhkemP521HkdfSha512: 18,
  DhkemSecp256k1HkdfSha256: 19,
  DhkemX25519HkdfSha256: 32,
  DhkemX448HkdfSha512: 33,
  HybridkemX25519Kyber768: 48,
  MlKem512: 64,
  MlKem768: 65,
  MlKem1024: 66,
  XWing: 25722
};
var KdfId = {
  HkdfSha256: 1,
  HkdfSha384: 2,
  HkdfSha512: 3,
  Sha3256: 4,
  Sha3384: 5,
  Sha3512: 6,
  Shake128: 16,
  Shake256: 17,
  TurboShake128: 18,
  TurboShake256: 19
};
var AeadId = {
  Aes128Gcm: 1,
  Aes256Gcm: 2,
  Chacha20Poly1305: 3,
  ExportOnly: 65535
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/consts.js
var INPUT_LENGTH_LIMIT = 8192;
var INFO_LENGTH_LIMIT = 268435456;
var MINIMUM_PSK_LENGTH = 32;
var EMPTY = /* @__PURE__ */ new Uint8Array(0);
var N_0 = 0n;
var N_1 = 1n;
var N_2 = 2n;

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/interfaces/kemInterface.js
var SUITE_ID_HEADER_KEM = /* @__PURE__ */ new Uint8Array([
  75,
  69,
  77,
  0,
  0
]);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/kdfs/hkdf.js
var HPKE_VERSION = /* @__PURE__ */ new Uint8Array([
  72,
  80,
  75,
  69,
  45,
  118,
  49
]);
function toUint8Array(input) {
  return new Uint8Array(toArrayBuffer(input));
}
function toArrayBuffer(input) {
  if (input instanceof ArrayBuffer) {
    return input;
  }
  if (ArrayBuffer.isView(input)) {
    return new Uint8Array(input.buffer, input.byteOffset, input.byteLength).slice().buffer;
  }
  return new Uint8Array(input).slice().buffer;
}
var HkdfNative = class extends NativeAlgorithm {
  constructor() {
    super();
    Object.defineProperty(this, "id", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: KdfId.HkdfSha256
    });
    Object.defineProperty(this, "hashSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "_suiteId", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: EMPTY
    });
    Object.defineProperty(this, "algHash", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: {
        name: "HMAC",
        hash: "SHA-256",
        length: 256
      }
    });
  }
  init(suiteId) {
    this._suiteId = suiteId;
  }
  buildLabeledIkm(label, ikm) {
    this._checkInit();
    const ret = new Uint8Array(7 + this._suiteId.byteLength + label.byteLength + ikm.byteLength);
    ret.set(HPKE_VERSION, 0);
    ret.set(this._suiteId, 7);
    ret.set(label, 7 + this._suiteId.byteLength);
    ret.set(ikm, 7 + this._suiteId.byteLength + label.byteLength);
    return ret;
  }
  buildLabeledInfo(label, info, len) {
    this._checkInit();
    const ret = new Uint8Array(9 + this._suiteId.byteLength + label.byteLength + info.byteLength);
    ret.set(new Uint8Array([0, len]), 0);
    ret.set(HPKE_VERSION, 2);
    ret.set(this._suiteId, 9);
    ret.set(label, 9 + this._suiteId.byteLength);
    ret.set(info, 9 + this._suiteId.byteLength + label.byteLength);
    return ret;
  }
  async extract(salt, ikm) {
    await this._setup();
    const saltBuf = salt.byteLength === 0 ? new ArrayBuffer(this.hashSize) : toArrayBuffer(salt);
    if (saltBuf.byteLength !== this.hashSize) {
      throw new InvalidParamError("The salt length must be the same as the hashSize");
    }
    const ikmBuf = toArrayBuffer(ikm);
    const key = await this._api.importKey("raw", saltBuf, this.algHash, false, [
      "sign"
    ]);
    return await this._api.sign("HMAC", key, ikmBuf);
  }
  async expand(prk, info, len) {
    await this._setup();
    const prkBuf = toArrayBuffer(prk);
    const key = await this._api.importKey("raw", prkBuf, this.algHash, false, [
      "sign"
    ]);
    const okm = new ArrayBuffer(len);
    const okmBytes = new Uint8Array(okm);
    let prev = EMPTY;
    const mid = toUint8Array(info);
    const tail = new Uint8Array(1);
    if (len > 255 * this.hashSize) {
      throw new Error("Entropy limit reached");
    }
    const tmp = new Uint8Array(this.hashSize + mid.length + 1);
    for (let i = 1, cur = 0; cur < okmBytes.length; i++) {
      tail[0] = i;
      tmp.set(prev, 0);
      tmp.set(mid, prev.length);
      tmp.set(tail, prev.length + mid.length);
      prev = new Uint8Array(await this._api.sign("HMAC", key, tmp.slice(0, prev.length + mid.length + 1)));
      if (okmBytes.length - cur >= prev.length) {
        okmBytes.set(prev, cur);
        cur += prev.length;
      } else {
        okmBytes.set(prev.slice(0, okmBytes.length - cur), cur);
        cur += okmBytes.length - cur;
      }
    }
    return okm;
  }
  async extractAndExpand(salt, ikm, info, len) {
    await this._setup();
    const ikmBuf = toArrayBuffer(ikm);
    const baseKey = await this._api.importKey("raw", ikmBuf, "HKDF", false, ["deriveBits"]);
    return await this._api.deriveBits({
      name: "HKDF",
      hash: this.algHash.hash,
      salt: toArrayBuffer(salt),
      info: toArrayBuffer(info)
    }, baseKey, len * 8);
  }
  async labeledExtract(salt, label, ikm) {
    return await this.extract(salt, this.buildLabeledIkm(label, ikm));
  }
  async labeledExpand(prk, label, info, len) {
    return await this.expand(prk, this.buildLabeledInfo(label, info, len), len);
  }
  _checkInit() {
    if (this._suiteId === EMPTY) {
      throw new Error("Not initialized. Call init()");
    }
  }
};
var HkdfSha256Native = class extends HkdfNative {
  constructor() {
    super(...arguments);
    Object.defineProperty(this, "id", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: KdfId.HkdfSha256
    });
    Object.defineProperty(this, "hashSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 32
    });
    Object.defineProperty(this, "algHash", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: {
        name: "HMAC",
        hash: "SHA-256",
        length: 256
      }
    });
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/utils/misc.js
var isCryptoKeyPair = (x) => typeof x === "object" && x !== null && typeof x.privateKey === "object" && typeof x.publicKey === "object";
function i2Osp(n, w) {
  if (w <= 0) {
    throw new Error("i2Osp: too small size");
  }
  if (n >= 256 ** w) {
    throw new Error("i2Osp: too large integer");
  }
  const ret = new Uint8Array(w);
  for (let i = 0; i < w && n; i++) {
    ret[w - (i + 1)] = n % 256;
    n = Math.floor(n / 256);
  }
  return ret;
}
function concat(a, b) {
  const ret = new Uint8Array(a.length + b.length);
  ret.set(a, 0);
  ret.set(b, a.length);
  return ret;
}
function base64UrlToBytes(v) {
  const base64 = v.replace(/-/g, "+").replace(/_/g, "/");
  const byteString = atob(base64);
  const ret = new Uint8Array(byteString.length);
  for (let i = 0; i < byteString.length; i++) {
    ret[i] = byteString.charCodeAt(i);
  }
  return ret;
}
async function loadCrypto() {
  if (typeof dntGlobalThis !== "undefined" && globalThis.crypto !== void 0) {
    return globalThis.crypto;
  }
  try {
    const { webcrypto } = await import("crypto");
    return webcrypto;
  } catch (_e) {
    throw new Error("failed to load Crypto");
  }
}
function xor(a, b) {
  if (a.byteLength !== b.byteLength) {
    throw new Error("xor: different length inputs");
  }
  const buf = new Uint8Array(a.byteLength);
  for (let i = 0; i < a.byteLength; i++) {
    buf[i] = a[i] ^ b[i];
  }
  return buf;
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/kems/dhkem.js
var LABEL_EAE_PRK = /* @__PURE__ */ new Uint8Array([
  101,
  97,
  101,
  95,
  112,
  114,
  107
]);
var LABEL_SHARED_SECRET = /* @__PURE__ */ new Uint8Array([
  115,
  104,
  97,
  114,
  101,
  100,
  95,
  115,
  101,
  99,
  114,
  101,
  116
]);
function concat3(a, b, c) {
  const ret = new Uint8Array(a.length + b.length + c.length);
  ret.set(a, 0);
  ret.set(b, a.length);
  ret.set(c, a.length + b.length);
  return ret;
}
var Dhkem = class {
  constructor(id, prim, kdf) {
    Object.defineProperty(this, "id", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "secretSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "encSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "publicKeySize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "privateKeySize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "_prim", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_kdf", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    this.id = id;
    this._prim = prim;
    this._kdf = kdf;
    const suiteId = new Uint8Array(SUITE_ID_HEADER_KEM);
    suiteId.set(i2Osp(this.id, 2), 3);
    this._kdf.init(suiteId);
  }
  async serializePublicKey(key) {
    return await this._prim.serializePublicKey(key);
  }
  async deserializePublicKey(key) {
    return await this._prim.deserializePublicKey(toArrayBuffer(key));
  }
  async serializePrivateKey(key) {
    return await this._prim.serializePrivateKey(key);
  }
  async deserializePrivateKey(key) {
    return await this._prim.deserializePrivateKey(toArrayBuffer(key));
  }
  async importKey(format, key, isPublic = true) {
    return await this._prim.importKey(format, key, isPublic);
  }
  async generateKeyPair() {
    return await this._prim.generateKeyPair();
  }
  async deriveKeyPair(ikm) {
    const rawIkm = toArrayBuffer(ikm);
    if (rawIkm.byteLength > INPUT_LENGTH_LIMIT) {
      throw new InvalidParamError("Too long ikm");
    }
    return await this._prim.deriveKeyPair(rawIkm);
  }
  async encap(params) {
    let ke;
    if (params.ekm === void 0) {
      ke = await this.generateKeyPair();
    } else if (isCryptoKeyPair(params.ekm)) {
      ke = params.ekm;
    } else {
      ke = await this.deriveKeyPair(params.ekm);
    }
    const enc = await this._prim.serializePublicKey(ke.publicKey);
    const pkrm = await this._prim.serializePublicKey(params.recipientPublicKey);
    try {
      let dh;
      if (params.senderKey === void 0) {
        dh = new Uint8Array(await this._prim.dh(ke.privateKey, params.recipientPublicKey));
      } else {
        const sks = isCryptoKeyPair(params.senderKey) ? params.senderKey.privateKey : params.senderKey;
        const dh1 = new Uint8Array(await this._prim.dh(ke.privateKey, params.recipientPublicKey));
        const dh2 = new Uint8Array(await this._prim.dh(sks, params.recipientPublicKey));
        dh = concat(dh1, dh2);
      }
      let kemContext;
      if (params.senderKey === void 0) {
        kemContext = concat(new Uint8Array(enc), new Uint8Array(pkrm));
      } else {
        const pks = isCryptoKeyPair(params.senderKey) ? params.senderKey.publicKey : await this._prim.derivePublicKey(params.senderKey);
        const pksm = await this._prim.serializePublicKey(pks);
        kemContext = concat3(new Uint8Array(enc), new Uint8Array(pkrm), new Uint8Array(pksm));
      }
      const sharedSecret = await this._generateSharedSecret(dh, kemContext);
      return {
        enc,
        sharedSecret
      };
    } catch (e) {
      throw new EncapError(e);
    }
  }
  async decap(params) {
    const enc = toArrayBuffer(params.enc);
    const pke = await this._prim.deserializePublicKey(enc);
    const skr = isCryptoKeyPair(params.recipientKey) ? params.recipientKey.privateKey : params.recipientKey;
    const pkr = isCryptoKeyPair(params.recipientKey) ? params.recipientKey.publicKey : await this._prim.derivePublicKey(params.recipientKey);
    const pkrm = await this._prim.serializePublicKey(pkr);
    try {
      let dh;
      if (params.senderPublicKey === void 0) {
        dh = new Uint8Array(await this._prim.dh(skr, pke));
      } else {
        const dh1 = new Uint8Array(await this._prim.dh(skr, pke));
        const dh2 = new Uint8Array(await this._prim.dh(skr, params.senderPublicKey));
        dh = concat(dh1, dh2);
      }
      let kemContext;
      if (params.senderPublicKey === void 0) {
        kemContext = concat(new Uint8Array(enc), new Uint8Array(pkrm));
      } else {
        const pksm = await this._prim.serializePublicKey(params.senderPublicKey);
        kemContext = new Uint8Array(enc.byteLength + pkrm.byteLength + pksm.byteLength);
        kemContext.set(new Uint8Array(enc), 0);
        kemContext.set(new Uint8Array(pkrm), enc.byteLength);
        kemContext.set(new Uint8Array(pksm), enc.byteLength + pkrm.byteLength);
      }
      return await this._generateSharedSecret(dh, kemContext);
    } catch (e) {
      throw new DecapError(e);
    }
  }
  async _generateSharedSecret(dh, kemContext) {
    const labeledIkm = this._kdf.buildLabeledIkm(LABEL_EAE_PRK, dh);
    const labeledInfo = this._kdf.buildLabeledInfo(LABEL_SHARED_SECRET, kemContext, this.secretSize);
    return await this._kdf.extractAndExpand(EMPTY, labeledIkm, labeledInfo, this.secretSize);
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/interfaces/dhkemPrimitives.js
var KEM_USAGES = ["deriveBits"];
var LABEL_DKP_PRK = /* @__PURE__ */ new Uint8Array([
  100,
  107,
  112,
  95,
  112,
  114,
  107
]);
var LABEL_SK = /* @__PURE__ */ new Uint8Array([115, 107]);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/kems/dhkemPrimitives/ec.js
var EC_P_521_PARAMS = {
  p: (1n << 521n) - 1n,
  b: 0x0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00n,
  gx: 0x00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66n,
  gy: 0x011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650n,
  coordinateSize: 66
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/xCryptoKey.js
var XCryptoKey = class {
  constructor(name, key, type, usages = []) {
    Object.defineProperty(this, "key", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "type", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "extractable", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: true
    });
    Object.defineProperty(this, "algorithm", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "usages", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    this.key = key;
    this.type = type;
    this.algorithm = { name };
    this.usages = usages;
    if (type === "public") {
      this.usages = [];
    }
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/kems/dhkemPrimitives/xCurve.js
var XCurveDhkemPrimitives = class {
  constructor(algName, keySize, curve, hkdf2) {
    Object.defineProperty(this, "_algName", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_curve", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_hkdf", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_nPk", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_nSk", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    this._algName = algName;
    this._nPk = keySize;
    this._nSk = keySize;
    this._curve = curve;
    this._hkdf = hkdf2;
  }
  serializePublicKey(key) {
    try {
      return Promise.resolve(key.key.buffer);
    } catch (e) {
      return Promise.reject(new SerializeError(e));
    }
  }
  async deserializePublicKey(key) {
    try {
      return await this._importRawKey(toArrayBuffer(key), true);
    } catch (e) {
      throw new DeserializeError(e);
    }
  }
  serializePrivateKey(key) {
    try {
      return Promise.resolve(key.key.buffer);
    } catch (e) {
      return Promise.reject(new SerializeError(e));
    }
  }
  async deserializePrivateKey(key) {
    try {
      return await this._importRawKey(toArrayBuffer(key), false);
    } catch (e) {
      throw new DeserializeError(e);
    }
  }
  async importKey(format, key, isPublic) {
    try {
      if (format === "raw") {
        return await this._importRawKey(key, isPublic);
      }
      if (key instanceof ArrayBuffer) {
        throw new Error("Invalid jwk key format");
      }
      return await this._importJWK(key, isPublic);
    } catch (e) {
      throw new DeserializeError(e);
    }
  }
  async generateKeyPair() {
    try {
      const rawSk = await this._curve.utils.randomSecretKey();
      const sk = new XCryptoKey(this._algName, rawSk, "private", KEM_USAGES);
      const pk = await this.derivePublicKey(sk);
      return { publicKey: pk, privateKey: sk };
    } catch (e) {
      throw new NotSupportedError(e);
    }
  }
  async deriveKeyPair(ikm) {
    try {
      const rawIkm = toArrayBuffer(ikm);
      const dkpPrk = await this._hkdf.labeledExtract(EMPTY.buffer, LABEL_DKP_PRK, new Uint8Array(rawIkm));
      const rawSk = await this._hkdf.labeledExpand(dkpPrk, LABEL_SK, EMPTY, this._nSk);
      const sk = new XCryptoKey(this._algName, new Uint8Array(rawSk), "private", KEM_USAGES);
      return {
        privateKey: sk,
        publicKey: await this.derivePublicKey(sk)
      };
    } catch (e) {
      throw new DeriveKeyPairError(e);
    }
  }
  derivePublicKey(key) {
    try {
      const pk = this._curve.getPublicKey(key.key);
      return Promise.resolve(new XCryptoKey(this._algName, pk, "public"));
    } catch (e) {
      return Promise.reject(new DeserializeError(e));
    }
  }
  dh(sk, pk) {
    try {
      return Promise.resolve(this._curve.getSharedSecret(sk.key, pk.key).buffer);
    } catch (e) {
      return Promise.reject(new SerializeError(e));
    }
  }
  _importRawKey(key, isPublic) {
    return new Promise((resolve, reject) => {
      if (isPublic && key.byteLength !== this._nPk) {
        reject(new Error("Invalid length of the key"));
      }
      if (!isPublic && key.byteLength !== this._nSk) {
        reject(new Error("Invalid length of the key"));
      }
      resolve(new XCryptoKey(this._algName, new Uint8Array(key), isPublic ? "public" : "private", isPublic ? [] : KEM_USAGES));
    });
  }
  _importJWK(key, isPublic) {
    return new Promise((resolve, reject) => {
      if (key.kty !== "OKP") {
        reject(new Error(`Invalid kty: ${key.kty}`));
      }
      if (key.crv !== this._algName) {
        reject(new Error(`Invalid crv: ${key.crv}`));
      }
      if (isPublic) {
        if (typeof key.d !== "undefined") {
          reject(new Error("Invalid key: `d` should not be set"));
        }
        if (typeof key.x !== "string") {
          reject(new Error("Invalid key: `x` not found"));
        }
        resolve(new XCryptoKey(this._algName, base64UrlToBytes(key.x), "public"));
      } else {
        if (typeof key.d !== "string") {
          reject(new Error("Invalid key: `d` not found"));
        }
        resolve(new XCryptoKey(this._algName, base64UrlToBytes(key.d), "private", KEM_USAGES));
      }
    });
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/utils/noble.js
function isBytes(a) {
  return a instanceof Uint8Array || ArrayBuffer.isView(a) && a.constructor.name === "Uint8Array";
}
function anumber(n, title = "") {
  if (!Number.isSafeInteger(n) || n < 0) {
    const prefix = title && `"${title}" `;
    throw new Error(`${prefix}expected integer >0, got ${n}`);
  }
}
function abytes(value, length, title = "") {
  const bytes = isBytes(value);
  const len = value?.length;
  const needsLen = length !== void 0;
  if (!bytes || needsLen && len !== length) {
    const prefix = title && `"${title}" `;
    const ofLen = needsLen ? ` of length ${length}` : "";
    const got = bytes ? `length=${len}` : `type=${typeof value}`;
    throw new Error(prefix + "expected Uint8Array" + ofLen + ", got " + got);
  }
  return value;
}
function aexists(instance, checkFinished = true) {
  if (instance.destroyed)
    throw new Error("Hash instance has been destroyed");
  if (checkFinished && instance.finished) {
    throw new Error("Hash#digest() has already been called");
  }
}
function aoutput(out, instance) {
  abytes(out, void 0, "digestInto() output");
  const min = instance.outputLen;
  if (out.length < min) {
    throw new Error('"digestInto() output" expected to be of length >=' + min);
  }
}
function abignumer(n) {
  if (typeof n === "bigint") {
    if (!isPosBig(n))
      throw new Error("positive bigint expected, got " + n);
  } else
    anumber(n);
  return n;
}
function u32(arr) {
  return new Uint32Array(arr.buffer, arr.byteOffset, Math.floor(arr.byteLength / 4));
}
function clean(...arrays) {
  for (let i = 0; i < arrays.length; i++) {
    arrays[i].fill(0);
  }
}
var _endianTestBuffer = /* @__PURE__ */ new Uint32Array([287454020]);
var _endianTestBytes = /* @__PURE__ */ new Uint8Array(_endianTestBuffer.buffer);
var isLE = _endianTestBytes[0] === 68;
function createView(arr) {
  return new DataView(arr.buffer, arr.byteOffset, arr.byteLength);
}
function rotr(word, shift) {
  return word << 32 - shift | word >>> shift;
}
var hasHexBuiltin = /* @__PURE__ */ (() => (
  // @ts-ignore: to use toHex
  typeof Uint8Array.from([]).toHex === "function" && // @ts-ignore: to use fromHex
  typeof Uint8Array.fromHex === "function"
))();
var hexes = /* @__PURE__ */ Array.from({ length: 256 }, (_, i) => i.toString(16).padStart(2, "0"));
var HEX_TO_BIGINT = [
  0n,
  1n,
  2n,
  3n,
  4n,
  5n,
  6n,
  7n,
  8n,
  9n,
  10n,
  11n,
  12n,
  13n,
  14n,
  15n
];
function bytesToHex(bytes) {
  abytes(bytes);
  if (hasHexBuiltin)
    return bytes.toHex();
  let hex = "";
  for (let i = 0; i < bytes.length; i++) {
    hex += hexes[bytes[i]];
  }
  return hex;
}
var asciis = { _0: 48, _9: 57, A: 65, F: 70, a: 97, f: 102 };
function asciiToBase16(ch) {
  if (ch >= asciis._0 && ch <= asciis._9)
    return ch - asciis._0;
  if (ch >= asciis.A && ch <= asciis.F)
    return ch - (asciis.A - 10);
  if (ch >= asciis.a && ch <= asciis.f)
    return ch - (asciis.a - 10);
  return;
}
function hexToBytes(hex) {
  if (typeof hex !== "string") {
    throw new Error("hex string expected, got " + typeof hex);
  }
  if (hasHexBuiltin)
    return Uint8Array.fromHex(hex);
  const hl = hex.length;
  const al = hl / 2;
  if (hl % 2) {
    throw new Error("hex string expected, got unpadded hex of length " + hl);
  }
  const array = new Uint8Array(al);
  for (let ai = 0, hi = 0; ai < al; ai++, hi += 2) {
    const n1 = asciiToBase16(hex.charCodeAt(hi));
    const n2 = asciiToBase16(hex.charCodeAt(hi + 1));
    if (n1 === void 0 || n2 === void 0) {
      const char = hex[hi] + hex[hi + 1];
      throw new Error('hex string expected, got non-hex character "' + char + '" at index ' + hi);
    }
    array[ai] = n1 * 16 + n2;
  }
  return array;
}
function hexToNumber(hex) {
  if (typeof hex !== "string") {
    throw new Error("hex string expected, got " + typeof hex);
  }
  let out = N_0;
  for (let i = 0; i < hex.length; i++) {
    const n = asciiToBase16(hex.charCodeAt(i));
    if (n === void 0) {
      throw new Error('hex string expected, got non-hex character "' + hex[i] + '" at index ' + i);
    }
    out = out << 4n | HEX_TO_BIGINT[n];
  }
  return out;
}
function numberToBigint(num) {
  anumber(num, "numberToBigint");
  let n = num;
  let out = N_0;
  let bit = 1n;
  while (n > 0) {
    if (n % 2 === 1)
      out += bit;
    n = Math.floor(n / 2);
    bit <<= 1n;
  }
  return out;
}
function bytesToNumberLE(bytes) {
  return hexToNumber(bytesToHex(copyBytes(abytes(bytes)).reverse()));
}
function numberToBytesBE(n, len) {
  anumber(len);
  n = abignumer(n);
  const res = hexToBytes(n.toString(16).padStart(len * 2, "0"));
  if (res.length !== len)
    throw new Error("number too large");
  return res;
}
function numberToBytesLE(n, len) {
  return numberToBytesBE(n, len).reverse();
}
function copyBytes(bytes) {
  return Uint8Array.from(bytes);
}
function isPosBig(n) {
  return typeof n === "bigint" && N_0 <= n;
}
function inRange(n, min, max) {
  return isPosBig(n) && isPosBig(min) && isPosBig(max) && min <= n && n < max;
}
function aInRange(title, n, min, max) {
  if (!inRange(n, min, max)) {
    throw new Error("expected valid " + title + ": " + min + " <= n < " + max + ", got " + n);
  }
}
function validateObject(object, fields = {}, optFields = {}) {
  if (!object || typeof object !== "object") {
    throw new Error("expected valid options object");
  }
  function checkField(fieldName, expectedType, isOpt) {
    const val = object[fieldName];
    if (isOpt && val === void 0)
      return;
    const current = typeof val;
    if (current !== expectedType || val === null) {
      throw new Error(`param "${fieldName}" is invalid: expected ${expectedType}, got ${current}`);
    }
  }
  const iter = (f, isOpt) => Object.entries(f).forEach(([k, v]) => checkField(k, v, isOpt));
  iter(fields, false);
  iter(optFields, true);
}
async function randomBytesAsync(bytesLength = 32) {
  const api = await loadCrypto();
  const rnd = new Uint8Array(bytesLength);
  api.getRandomValues(rnd);
  return rnd;
}
function oidNist(suffix) {
  return {
    oid: Uint8Array.from([
      6,
      9,
      96,
      134,
      72,
      1,
      101,
      3,
      4,
      2,
      suffix
    ])
  };
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/hash/hash.js
function ahash(h) {
  if (typeof h !== "function" || typeof h.create !== "function") {
    throw new Error("Hash must wrapped by utils.createHasher");
  }
  anumber(h.outputLen);
  anumber(h.blockLen);
}
function createHasher(hashCons, info = {}) {
  const hashFn = (msg, opts) => hashCons(opts).update(msg).digest();
  const tmp = hashCons(void 0);
  const hashC = Object.assign(hashFn, {
    outputLen: tmp.outputLen,
    blockLen: tmp.blockLen,
    create: (opts) => hashCons(opts),
    ...info
  });
  return Object.freeze(hashC);
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/hash/hmac.js
var _HMAC = class {
  constructor(hash, key) {
    Object.defineProperty(this, "oHash", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "iHash", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "blockLen", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "outputLen", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "finished", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: false
    });
    Object.defineProperty(this, "destroyed", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: false
    });
    ahash(hash);
    abytes(key, void 0, "key");
    this.iHash = hash.create();
    if (typeof this.iHash.update !== "function") {
      throw new Error("Expected instance of class which extends utils.Hash");
    }
    this.blockLen = this.iHash.blockLen;
    this.outputLen = this.iHash.outputLen;
    const blockLen = this.blockLen;
    const pad = new Uint8Array(blockLen);
    pad.set(key.length > blockLen ? hash.create().update(key).digest() : key);
    for (let i = 0; i < pad.length; i++)
      pad[i] ^= 54;
    this.iHash.update(pad);
    this.oHash = hash.create();
    for (let i = 0; i < pad.length; i++)
      pad[i] ^= 54 ^ 92;
    this.oHash.update(pad);
    clean(pad);
  }
  update(buf) {
    aexists(this);
    this.iHash.update(buf);
    return this;
  }
  digestInto(out) {
    aexists(this);
    abytes(out, this.outputLen, "output");
    this.finished = true;
    this.iHash.digestInto(out);
    this.oHash.update(out);
    this.oHash.digestInto(out);
    this.destroy();
  }
  digest() {
    const out = new Uint8Array(this.oHash.outputLen);
    this.digestInto(out);
    return out;
  }
  _cloneInto(to) {
    to ||= Object.create(Object.getPrototypeOf(this), {});
    const { oHash, iHash, finished, destroyed, blockLen, outputLen } = this;
    to = to;
    to.finished = finished;
    to.destroyed = destroyed;
    to.blockLen = blockLen;
    to.outputLen = outputLen;
    to.oHash = oHash._cloneInto(to.oHash);
    to.iHash = iHash._cloneInto(to.iHash);
    return to;
  }
  clone() {
    return this._cloneInto();
  }
  destroy() {
    this.destroyed = true;
    this.oHash.destroy();
    this.iHash.destroy();
  }
};
var hmac = (hash, key, message) => new _HMAC(hash, key).update(message).digest();
hmac.create = (hash, key) => new _HMAC(hash, key);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/hash/md.js
function Chi(a, b, c) {
  return a & b ^ ~a & c;
}
function Maj(a, b, c) {
  return a & b ^ a & c ^ b & c;
}
var HashMD = class {
  constructor(blockLen, outputLen, padOffset, isLE3) {
    Object.defineProperty(this, "blockLen", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "outputLen", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "padOffset", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "isLE", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "buffer", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "view", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "finished", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: false
    });
    Object.defineProperty(this, "length", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "pos", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "destroyed", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: false
    });
    this.blockLen = blockLen;
    this.outputLen = outputLen;
    this.padOffset = padOffset;
    this.isLE = isLE3;
    this.buffer = new Uint8Array(blockLen);
    this.view = createView(this.buffer);
  }
  update(data) {
    aexists(this);
    abytes(data);
    const { view, buffer, blockLen } = this;
    const len = data.length;
    for (let pos = 0; pos < len; ) {
      const take = Math.min(blockLen - this.pos, len - pos);
      if (take === blockLen) {
        const dataView = createView(data);
        for (; blockLen <= len - pos; pos += blockLen) {
          this.process(dataView, pos);
        }
        continue;
      }
      buffer.set(data.subarray(pos, pos + take), this.pos);
      this.pos += take;
      pos += take;
      if (this.pos === blockLen) {
        this.process(view, 0);
        this.pos = 0;
      }
    }
    this.length += data.length;
    this.roundClean();
    return this;
  }
  digestInto(out) {
    aexists(this);
    aoutput(out, this);
    this.finished = true;
    const { buffer, view, blockLen, isLE: isLE3 } = this;
    let { pos } = this;
    buffer[pos++] = 128;
    clean(this.buffer.subarray(pos));
    if (this.padOffset > blockLen - pos) {
      this.process(view, 0);
      pos = 0;
    }
    for (let i = pos; i < blockLen; i++)
      buffer[i] = 0;
    view.setBigUint64(blockLen - 8, numberToBigint(this.length * 8), isLE3);
    this.process(view, 0);
    const oview = createView(out);
    const len = this.outputLen;
    if (len % 4)
      throw new Error("_sha2: outputLen must be aligned to 32bit");
    const outLen = len / 4;
    const state = this.get();
    if (outLen > state.length) {
      throw new Error("_sha2: outputLen bigger than state");
    }
    for (let i = 0; i < outLen; i++)
      oview.setUint32(4 * i, state[i], isLE3);
  }
  digest() {
    const { buffer, outputLen } = this;
    this.digestInto(buffer);
    const res = buffer.slice(0, outputLen);
    this.destroy();
    return res;
  }
  _cloneInto(to) {
    to ||= new this.constructor();
    to.set(...this.get());
    const { blockLen, buffer, length, finished, destroyed, pos } = this;
    to.destroyed = destroyed;
    to.finished = finished;
    to.length = length;
    to.pos = pos;
    if (length % blockLen)
      to.buffer.set(buffer);
    return to;
  }
  clone() {
    return this._cloneInto();
  }
};
var SHA256_IV = /* @__PURE__ */ Uint32Array.from([
  1779033703,
  3144134277,
  1013904242,
  2773480762,
  1359893119,
  2600822924,
  528734635,
  1541459225
]);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/hash/u64.js
var U32_MASK64 = 0xffffffffn;
var _32n = 32n;
function fromBig(n, le = false) {
  if (le) {
    return { h: Number(n & U32_MASK64), l: Number(n >> _32n & U32_MASK64) };
  }
  return {
    h: Number(n >> _32n & U32_MASK64) | 0,
    l: Number(n & U32_MASK64) | 0
  };
}
function split(lst, le = false) {
  const len = lst.length;
  const Ah = new Uint32Array(len);
  const Al = new Uint32Array(len);
  for (let i = 0; i < len; i++) {
    const { h, l } = fromBig(lst[i], le);
    [Ah[i], Al[i]] = [h, l];
  }
  return [Ah, Al];
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/hash/sha2.js
var SHA256_K = /* @__PURE__ */ Uint32Array.from([
  1116352408,
  1899447441,
  3049323471,
  3921009573,
  961987163,
  1508970993,
  2453635748,
  2870763221,
  3624381080,
  310598401,
  607225278,
  1426881987,
  1925078388,
  2162078206,
  2614888103,
  3248222580,
  3835390401,
  4022224774,
  264347078,
  604807628,
  770255983,
  1249150122,
  1555081692,
  1996064986,
  2554220882,
  2821834349,
  2952996808,
  3210313671,
  3336571891,
  3584528711,
  113926993,
  338241895,
  666307205,
  773529912,
  1294757372,
  1396182291,
  1695183700,
  1986661051,
  2177026350,
  2456956037,
  2730485921,
  2820302411,
  3259730800,
  3345764771,
  3516065817,
  3600352804,
  4094571909,
  275423344,
  430227734,
  506948616,
  659060556,
  883997877,
  958139571,
  1322822218,
  1537002063,
  1747873779,
  1955562222,
  2024104815,
  2227730452,
  2361852424,
  2428436474,
  2756734187,
  3204031479,
  3329325298
]);
var SHA256_W = /* @__PURE__ */ new Uint32Array(64);
var SHA2_32B = class extends HashMD {
  constructor(outputLen) {
    super(64, outputLen, 8, false);
  }
  get() {
    const { A, B, C, D, E, F, G, H } = this;
    return [A, B, C, D, E, F, G, H];
  }
  set(A, B, C, D, E, F, G, H) {
    this.A = A | 0;
    this.B = B | 0;
    this.C = C | 0;
    this.D = D | 0;
    this.E = E | 0;
    this.F = F | 0;
    this.G = G | 0;
    this.H = H | 0;
  }
  process(view, offset) {
    for (let i = 0; i < 16; i++, offset += 4) {
      SHA256_W[i] = view.getUint32(offset, false);
    }
    for (let i = 16; i < 64; i++) {
      const W15 = SHA256_W[i - 15];
      const W2 = SHA256_W[i - 2];
      const s0 = rotr(W15, 7) ^ rotr(W15, 18) ^ W15 >>> 3;
      const s1 = rotr(W2, 17) ^ rotr(W2, 19) ^ W2 >>> 10;
      SHA256_W[i] = s1 + SHA256_W[i - 7] + s0 + SHA256_W[i - 16] | 0;
    }
    let { A, B, C, D, E, F, G, H } = this;
    for (let i = 0; i < 64; i++) {
      const sigma1 = rotr(E, 6) ^ rotr(E, 11) ^ rotr(E, 25);
      const T1 = H + sigma1 + Chi(E, F, G) + SHA256_K[i] + SHA256_W[i] | 0;
      const sigma0 = rotr(A, 2) ^ rotr(A, 13) ^ rotr(A, 22);
      const T2 = sigma0 + Maj(A, B, C) | 0;
      H = G;
      G = F;
      F = E;
      E = D + T1 | 0;
      D = C;
      C = B;
      B = A;
      A = T1 + T2 | 0;
    }
    A = A + this.A | 0;
    B = B + this.B | 0;
    C = C + this.C | 0;
    D = D + this.D | 0;
    E = E + this.E | 0;
    F = F + this.F | 0;
    G = G + this.G | 0;
    H = H + this.H | 0;
    this.set(A, B, C, D, E, F, G, H);
  }
  roundClean() {
    clean(SHA256_W);
  }
  destroy() {
    this.set(0, 0, 0, 0, 0, 0, 0, 0);
    clean(this.buffer);
  }
};
var _SHA256 = class extends SHA2_32B {
  constructor() {
    super(32);
    Object.defineProperty(this, "A", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[0] | 0
    });
    Object.defineProperty(this, "B", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[1] | 0
    });
    Object.defineProperty(this, "C", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[2] | 0
    });
    Object.defineProperty(this, "D", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[3] | 0
    });
    Object.defineProperty(this, "E", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[4] | 0
    });
    Object.defineProperty(this, "F", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[5] | 0
    });
    Object.defineProperty(this, "G", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[6] | 0
    });
    Object.defineProperty(this, "H", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: SHA256_IV[7] | 0
    });
  }
};
var sha256 = /* @__PURE__ */ createHasher(
  () => new _SHA256(),
  /* @__PURE__ */ oidNist(1)
);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/hash/sha3.js
var _0n = 0n;
var _1n = 1n;
var _2n = 2n;
var _7n = 7n;
var _256n = 256n;
var _0x71n = 0x71n;
var SHA3_PI = [];
var SHA3_ROTL = [];
var _SHA3_IOTA = [];
for (let round = 0, R = _1n, x = 1, y = 0; round < 24; round++) {
  [x, y] = [y, (2 * x + 3 * y) % 5];
  SHA3_PI.push(2 * (5 * y + x));
  SHA3_ROTL.push((round + 1) * (round + 2) / 2 % 64);
  let t = _0n;
  for (let j = 0; j < 7; j++) {
    R = (R << _1n ^ (R >> _7n) * _0x71n) % _256n;
    if (R & _2n)
      t ^= _1n << (_1n << BigInt(j)) - _1n;
  }
  _SHA3_IOTA.push(t);
}
var IOTAS = split(_SHA3_IOTA, true);
var SHA3_IOTA_H = IOTAS[0];
var SHA3_IOTA_L = IOTAS[1];

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/curve/modular.js
function mod(a, b) {
  const result = a % b;
  return result >= N_0 ? result : b + result;
}
function pow2(x, power, modulo) {
  let res = x;
  while (power-- > N_0) {
    res *= res;
    res %= modulo;
  }
  return res;
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/curve/curve.js
function createKeygen(randomSecretKey, getPublicKey) {
  return function keygen(seed) {
    const secretKey = randomSecretKey(seed);
    return { secretKey, publicKey: getPublicKey(secretKey) };
  };
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/common/esm/src/curve/montgomery.js
function validateOpts(curve) {
  validateObject(curve, {
    adjustScalarBytes: "function",
    powPminus2: "function"
  });
  return Object.freeze({ ...curve });
}
function montgomery(curveDef) {
  const CURVE = validateOpts(curveDef);
  const { P, type, adjustScalarBytes: adjustScalarBytes2, powPminus2, randomBytes: rand } = CURVE;
  const is25519 = type === "x25519";
  if (!is25519 && type !== "x448")
    throw new Error("invalid type");
  const randomBytes_ = rand || randomBytesAsync;
  const montgomeryBits = is25519 ? 255n : 448n;
  const fieldLen = is25519 ? 32 : 56;
  const Gu = is25519 ? 9n : 5n;
  const a24 = is25519 ? 121665n : 39081n;
  const minScalar = is25519 ? N_2 ** 254n : N_2 ** 447n;
  const maxAdded = is25519 ? 8n * N_2 ** 251n - N_1 : 4n * N_2 ** 445n - N_1;
  const maxScalar = minScalar + maxAdded + N_1;
  const modP = (n) => mod(n, P);
  const GuBytes = encodeU(Gu);
  function encodeU(u) {
    return numberToBytesLE(modP(u), fieldLen);
  }
  function decodeU(u) {
    const _u = copyBytes(abytes(u, fieldLen, "uCoordinate"));
    if (is25519)
      _u[31] &= 127;
    return modP(bytesToNumberLE(_u));
  }
  function decodeScalar(scalar) {
    return bytesToNumberLE(adjustScalarBytes2(copyBytes(abytes(scalar, fieldLen, "scalar"))));
  }
  function scalarMult(scalar, u) {
    const pu = montgomeryLadder(decodeU(u), decodeScalar(scalar));
    if (pu === N_0)
      throw new Error("invalid private or public key received");
    return encodeU(pu);
  }
  function scalarMultBase(scalar) {
    return scalarMult(scalar, GuBytes);
  }
  const getPublicKey = scalarMultBase;
  const getSharedSecret = scalarMult;
  function cswap(swap, x_2, x_3) {
    const dummy = modP(swap * (x_2 - x_3));
    x_2 = modP(x_2 - dummy);
    x_3 = modP(x_3 + dummy);
    return { x_2, x_3 };
  }
  function montgomeryLadder(u, scalar) {
    aInRange("u", u, N_0, P);
    aInRange("scalar", scalar, minScalar, maxScalar);
    const k = scalar;
    const x_1 = u;
    let x_2 = N_1;
    let z_2 = N_0;
    let x_3 = u;
    let z_3 = N_1;
    let swap = N_0;
    for (let t = montgomeryBits - 1n; t >= N_0; t--) {
      const k_t = k >> t & N_1;
      swap ^= k_t;
      ({ x_2, x_3 } = cswap(swap, x_2, x_3));
      ({ x_2: z_2, x_3: z_3 } = cswap(swap, z_2, z_3));
      swap = k_t;
      const A = x_2 + z_2;
      const AA = modP(A * A);
      const B = x_2 - z_2;
      const BB = modP(B * B);
      const E = AA - BB;
      const C = x_3 + z_3;
      const D = x_3 - z_3;
      const DA = modP(D * A);
      const CB = modP(C * B);
      const dacb = DA + CB;
      const da_cb = DA - CB;
      x_3 = modP(dacb * dacb);
      z_3 = modP(x_1 * modP(da_cb * da_cb));
      x_2 = modP(AA * BB);
      z_2 = modP(E * (AA + modP(a24 * E)));
    }
    ({ x_2, x_3 } = cswap(swap, x_2, x_3));
    ({ x_2: z_2, x_3: z_3 } = cswap(swap, z_2, z_3));
    const z2 = powPminus2(z_2);
    return modP(x_2 * z2);
  }
  const lengths = {
    secretKey: fieldLen,
    publicKey: fieldLen,
    seed: fieldLen
  };
  const randomSecretKey = async (seed) => {
    if (seed === void 0) {
      seed = await randomBytes_(fieldLen);
    }
    abytes(seed, lengths.seed, "seed");
    return seed;
  };
  const utils = { randomSecretKey };
  return Object.freeze({
    keygen: createKeygen(randomSecretKey, getPublicKey),
    getSharedSecret,
    getPublicKey,
    scalarMult,
    scalarMultBase,
    utils,
    GuBytes: GuBytes.slice(),
    lengths
  });
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/chacha20poly1305/esm/src/chacha/utils.js
function abool(b) {
  if (typeof b !== "boolean")
    throw new Error(`boolean expected, not ${b}`);
}
var wrapCipher = /* @__NO_SIDE_EFFECTS__ */ (params, constructor) => {
  function wrappedCipher(key, ...args) {
    abytes(key, void 0, "key");
    if (!isLE) {
      throw new Error("Non little-endian hardware is not yet supported");
    }
    if (params.nonceLength !== void 0) {
      const nonce = args[0];
      abytes(nonce, params.varSizeNonce ? void 0 : params.nonceLength, "nonce");
    }
    const tagl = params.tagLength;
    if (tagl && args[1] !== void 0)
      abytes(args[1], void 0, "AAD");
    const cipher = constructor(key, ...args);
    const checkOutput = (fnLength, output) => {
      if (output !== void 0) {
        if (fnLength !== 2)
          throw new Error("cipher output not supported");
        abytes(output, void 0, "output");
      }
    };
    let called = false;
    const wrCipher = {
      encrypt(data, output) {
        if (called) {
          throw new Error("cannot encrypt() twice with same key + nonce");
        }
        called = true;
        abytes(data);
        checkOutput(cipher.encrypt.length, output);
        return cipher.encrypt(data, output);
      },
      decrypt(data, output) {
        abytes(data);
        if (tagl && data.length < tagl) {
          throw new Error('"ciphertext" expected length bigger than tagLength=' + tagl);
        }
        checkOutput(cipher.decrypt.length, output);
        return cipher.decrypt(data, output);
      }
    };
    return wrCipher;
  }
  Object.assign(wrappedCipher, params);
  return wrappedCipher;
};
function checkOpts(defaults, opts) {
  if (opts == null || typeof opts !== "object") {
    throw new Error("options must be defined");
  }
  const merged = Object.assign(defaults, opts);
  return merged;
}
function equalBytes(a, b) {
  if (a.length !== b.length)
    return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++)
    diff |= a[i] ^ b[i];
  return diff === 0;
}
function getOutput(expectedLength, out, onlyAligned = true) {
  if (out === void 0)
    return new Uint8Array(expectedLength);
  if (out.length !== expectedLength) {
    throw new Error('"output" expected Uint8Array of length ' + expectedLength + ", got: " + out.length);
  }
  if (onlyAligned && !isAligned32(out)) {
    throw new Error("invalid output, must be aligned");
  }
  return out;
}
function u64Lengths(dataLength, aadLength, isLE3) {
  abool(isLE3);
  const num = new Uint8Array(16);
  const view = createView(num);
  view.setBigUint64(0, numberToBigint(aadLength), isLE3);
  view.setBigUint64(8, numberToBigint(dataLength), isLE3);
  return num;
}
function isAligned32(bytes) {
  return bytes.byteOffset % 4 === 0;
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/chacha20poly1305/esm/src/chacha/_arx.js
var _utf8ToBytes = (str) => Uint8Array.from(str.split("").map((c) => c.charCodeAt(0)));
var sigma16 = _utf8ToBytes("expand 16-byte k");
var sigma32 = _utf8ToBytes("expand 32-byte k");
var sigma16_32 = u32(sigma16);
var sigma32_32 = u32(sigma32);
function rotl(a, b) {
  return a << b | a >>> 32 - b;
}
function isAligned322(b) {
  return b.byteOffset % 4 === 0;
}
var BLOCK_LEN = 64;
var BLOCK_LEN32 = 16;
var MAX_COUNTER = 2 ** 32 - 1;
var U32_EMPTY = Uint32Array.of();
function runCipher(core, sigma, key, nonce, data, output, counter, rounds) {
  const len = data.length;
  const block = new Uint8Array(BLOCK_LEN);
  const b32 = u32(block);
  const isAligned = isAligned322(data) && isAligned322(output);
  const d32 = isAligned ? u32(data) : U32_EMPTY;
  const o32 = isAligned ? u32(output) : U32_EMPTY;
  for (let pos = 0; pos < len; counter++) {
    core(sigma, key, nonce, b32, counter, rounds);
    if (counter >= MAX_COUNTER)
      throw new Error("arx: counter overflow");
    const take = Math.min(BLOCK_LEN, len - pos);
    if (isAligned && take === BLOCK_LEN) {
      const pos32 = pos / 4;
      if (pos % 4 !== 0)
        throw new Error("arx: invalid block position");
      for (let j = 0, posj; j < BLOCK_LEN32; j++) {
        posj = pos32 + j;
        o32[posj] = d32[posj] ^ b32[j];
      }
      pos += BLOCK_LEN;
      continue;
    }
    for (let j = 0, posj; j < take; j++) {
      posj = pos + j;
      output[posj] = data[posj] ^ block[j];
    }
    pos += take;
  }
}
function createCipher(core, opts) {
  const { allowShortKeys, extendNonceFn, counterLength, counterRight, rounds } = checkOpts({
    allowShortKeys: false,
    counterLength: 8,
    counterRight: false,
    rounds: 20
  }, opts);
  if (typeof core !== "function")
    throw new Error("core must be a function");
  anumber(counterLength);
  anumber(rounds);
  abool(counterRight);
  abool(allowShortKeys);
  return (key, nonce, data, output, counter = 0) => {
    abytes(key, void 0, "key");
    abytes(nonce, void 0, "nonce");
    abytes(data, void 0, "data");
    const len = data.length;
    if (output === void 0)
      output = new Uint8Array(len);
    abytes(output, void 0, "output");
    anumber(counter);
    if (counter < 0 || counter >= MAX_COUNTER) {
      throw new Error("arx: counter overflow");
    }
    if (output.length < len) {
      throw new Error(`arx: output (${output.length}) is shorter than data (${len})`);
    }
    const toClean = [];
    const l = key.length;
    let k;
    let sigma;
    if (l === 32) {
      toClean.push(k = copyBytes(key));
      sigma = sigma32_32;
    } else if (l === 16 && allowShortKeys) {
      k = new Uint8Array(32);
      k.set(key);
      k.set(key, 16);
      sigma = sigma16_32;
      toClean.push(k);
    } else {
      abytes(key, 32, "arx key");
      throw new Error("invalid key size");
    }
    if (!isAligned322(nonce))
      toClean.push(nonce = copyBytes(nonce));
    const k32 = u32(k);
    if (extendNonceFn) {
      if (nonce.length !== 24) {
        throw new Error(`arx: extended nonce must be 24 bytes`);
      }
      extendNonceFn(sigma, k32, u32(nonce.subarray(0, 16)), k32);
      nonce = nonce.subarray(16);
    }
    const nonceNcLen = 16 - counterLength;
    if (nonceNcLen !== nonce.length) {
      throw new Error(`arx: nonce must be ${nonceNcLen} or 16 bytes`);
    }
    if (nonceNcLen !== 12) {
      const nc = new Uint8Array(12);
      nc.set(nonce, counterRight ? 0 : 12 - nonce.length);
      nonce = nc;
      toClean.push(nonce);
    }
    const n32 = u32(nonce);
    runCipher(core, sigma, k32, n32, data, output, counter, rounds);
    clean(...toClean);
    return output;
  };
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/chacha20poly1305/esm/src/chacha/_poly1305.js
function u8to16(a, i) {
  return a[i++] & 255 | (a[i++] & 255) << 8;
}
var Poly1305 = class {
  // Can be speed-up using BigUint64Array, at the cost of complexity
  constructor(key) {
    Object.defineProperty(this, "blockLen", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 16
    });
    Object.defineProperty(this, "outputLen", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 16
    });
    Object.defineProperty(this, "buffer", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: new Uint8Array(16)
    });
    Object.defineProperty(this, "r", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: new Uint16Array(10)
    });
    Object.defineProperty(this, "h", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: new Uint16Array(10)
    });
    Object.defineProperty(this, "pad", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: new Uint16Array(8)
    });
    Object.defineProperty(this, "pos", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 0
    });
    Object.defineProperty(this, "finished", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: false
    });
    key = copyBytes(abytes(key, 32, "key"));
    const t0 = u8to16(key, 0);
    const t1 = u8to16(key, 2);
    const t2 = u8to16(key, 4);
    const t3 = u8to16(key, 6);
    const t4 = u8to16(key, 8);
    const t5 = u8to16(key, 10);
    const t6 = u8to16(key, 12);
    const t7 = u8to16(key, 14);
    this.r[0] = t0 & 8191;
    this.r[1] = (t0 >>> 13 | t1 << 3) & 8191;
    this.r[2] = (t1 >>> 10 | t2 << 6) & 7939;
    this.r[3] = (t2 >>> 7 | t3 << 9) & 8191;
    this.r[4] = (t3 >>> 4 | t4 << 12) & 255;
    this.r[5] = t4 >>> 1 & 8190;
    this.r[6] = (t4 >>> 14 | t5 << 2) & 8191;
    this.r[7] = (t5 >>> 11 | t6 << 5) & 8065;
    this.r[8] = (t6 >>> 8 | t7 << 8) & 8191;
    this.r[9] = t7 >>> 5 & 127;
    for (let i = 0; i < 8; i++)
      this.pad[i] = u8to16(key, 16 + 2 * i);
  }
  process(data, offset, isLast = false) {
    const hibit = isLast ? 0 : 1 << 11;
    const { h, r } = this;
    const r0 = r[0];
    const r1 = r[1];
    const r2 = r[2];
    const r3 = r[3];
    const r4 = r[4];
    const r5 = r[5];
    const r6 = r[6];
    const r7 = r[7];
    const r8 = r[8];
    const r9 = r[9];
    const t0 = u8to16(data, offset + 0);
    const t1 = u8to16(data, offset + 2);
    const t2 = u8to16(data, offset + 4);
    const t3 = u8to16(data, offset + 6);
    const t4 = u8to16(data, offset + 8);
    const t5 = u8to16(data, offset + 10);
    const t6 = u8to16(data, offset + 12);
    const t7 = u8to16(data, offset + 14);
    const h0 = h[0] + (t0 & 8191);
    const h1 = h[1] + ((t0 >>> 13 | t1 << 3) & 8191);
    const h2 = h[2] + ((t1 >>> 10 | t2 << 6) & 8191);
    const h3 = h[3] + ((t2 >>> 7 | t3 << 9) & 8191);
    const h4 = h[4] + ((t3 >>> 4 | t4 << 12) & 8191);
    const h5 = h[5] + (t4 >>> 1 & 8191);
    const h6 = h[6] + ((t4 >>> 14 | t5 << 2) & 8191);
    const h7 = h[7] + ((t5 >>> 11 | t6 << 5) & 8191);
    const h8 = h[8] + ((t6 >>> 8 | t7 << 8) & 8191);
    const h9 = h[9] + (t7 >>> 5 | hibit);
    let c = 0;
    let d0 = c + h0 * r0 + h1 * (5 * r9) + h2 * (5 * r8) + h3 * (5 * r7) + h4 * (5 * r6);
    c = d0 >>> 13;
    d0 &= 8191;
    d0 += h5 * (5 * r5) + h6 * (5 * r4) + h7 * (5 * r3) + h8 * (5 * r2) + h9 * (5 * r1);
    c += d0 >>> 13;
    d0 &= 8191;
    let d1 = c + h0 * r1 + h1 * r0 + h2 * (5 * r9) + h3 * (5 * r8) + h4 * (5 * r7);
    c = d1 >>> 13;
    d1 &= 8191;
    d1 += h5 * (5 * r6) + h6 * (5 * r5) + h7 * (5 * r4) + h8 * (5 * r3) + h9 * (5 * r2);
    c += d1 >>> 13;
    d1 &= 8191;
    let d2 = c + h0 * r2 + h1 * r1 + h2 * r0 + h3 * (5 * r9) + h4 * (5 * r8);
    c = d2 >>> 13;
    d2 &= 8191;
    d2 += h5 * (5 * r7) + h6 * (5 * r6) + h7 * (5 * r5) + h8 * (5 * r4) + h9 * (5 * r3);
    c += d2 >>> 13;
    d2 &= 8191;
    let d3 = c + h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * (5 * r9);
    c = d3 >>> 13;
    d3 &= 8191;
    d3 += h5 * (5 * r8) + h6 * (5 * r7) + h7 * (5 * r6) + h8 * (5 * r5) + h9 * (5 * r4);
    c += d3 >>> 13;
    d3 &= 8191;
    let d4 = c + h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;
    c = d4 >>> 13;
    d4 &= 8191;
    d4 += h5 * (5 * r9) + h6 * (5 * r8) + h7 * (5 * r7) + h8 * (5 * r6) + h9 * (5 * r5);
    c += d4 >>> 13;
    d4 &= 8191;
    let d5 = c + h0 * r5 + h1 * r4 + h2 * r3 + h3 * r2 + h4 * r1;
    c = d5 >>> 13;
    d5 &= 8191;
    d5 += h5 * r0 + h6 * (5 * r9) + h7 * (5 * r8) + h8 * (5 * r7) + h9 * (5 * r6);
    c += d5 >>> 13;
    d5 &= 8191;
    let d6 = c + h0 * r6 + h1 * r5 + h2 * r4 + h3 * r3 + h4 * r2;
    c = d6 >>> 13;
    d6 &= 8191;
    d6 += h5 * r1 + h6 * r0 + h7 * (5 * r9) + h8 * (5 * r8) + h9 * (5 * r7);
    c += d6 >>> 13;
    d6 &= 8191;
    let d7 = c + h0 * r7 + h1 * r6 + h2 * r5 + h3 * r4 + h4 * r3;
    c = d7 >>> 13;
    d7 &= 8191;
    d7 += h5 * r2 + h6 * r1 + h7 * r0 + h8 * (5 * r9) + h9 * (5 * r8);
    c += d7 >>> 13;
    d7 &= 8191;
    let d8 = c + h0 * r8 + h1 * r7 + h2 * r6 + h3 * r5 + h4 * r4;
    c = d8 >>> 13;
    d8 &= 8191;
    d8 += h5 * r3 + h6 * r2 + h7 * r1 + h8 * r0 + h9 * (5 * r9);
    c += d8 >>> 13;
    d8 &= 8191;
    let d9 = c + h0 * r9 + h1 * r8 + h2 * r7 + h3 * r6 + h4 * r5;
    c = d9 >>> 13;
    d9 &= 8191;
    d9 += h5 * r4 + h6 * r3 + h7 * r2 + h8 * r1 + h9 * r0;
    c += d9 >>> 13;
    d9 &= 8191;
    c = (c << 2) + c | 0;
    c = c + d0 | 0;
    d0 = c & 8191;
    c = c >>> 13;
    d1 += c;
    h[0] = d0;
    h[1] = d1;
    h[2] = d2;
    h[3] = d3;
    h[4] = d4;
    h[5] = d5;
    h[6] = d6;
    h[7] = d7;
    h[8] = d8;
    h[9] = d9;
  }
  finalize() {
    const { h, pad } = this;
    const g = new Uint16Array(10);
    let c = h[1] >>> 13;
    h[1] &= 8191;
    for (let i = 2; i < 10; i++) {
      h[i] += c;
      c = h[i] >>> 13;
      h[i] &= 8191;
    }
    h[0] += c * 5;
    c = h[0] >>> 13;
    h[0] &= 8191;
    h[1] += c;
    c = h[1] >>> 13;
    h[1] &= 8191;
    h[2] += c;
    g[0] = h[0] + 5;
    c = g[0] >>> 13;
    g[0] &= 8191;
    for (let i = 1; i < 10; i++) {
      g[i] = h[i] + c;
      c = g[i] >>> 13;
      g[i] &= 8191;
    }
    g[9] -= 1 << 13;
    let mask = (c ^ 1) - 1;
    for (let i = 0; i < 10; i++)
      g[i] &= mask;
    mask = ~mask;
    for (let i = 0; i < 10; i++)
      h[i] = h[i] & mask | g[i];
    h[0] = (h[0] | h[1] << 13) & 65535;
    h[1] = (h[1] >>> 3 | h[2] << 10) & 65535;
    h[2] = (h[2] >>> 6 | h[3] << 7) & 65535;
    h[3] = (h[3] >>> 9 | h[4] << 4) & 65535;
    h[4] = (h[4] >>> 12 | h[5] << 1 | h[6] << 14) & 65535;
    h[5] = (h[6] >>> 2 | h[7] << 11) & 65535;
    h[6] = (h[7] >>> 5 | h[8] << 8) & 65535;
    h[7] = (h[8] >>> 8 | h[9] << 5) & 65535;
    let f = h[0] + pad[0];
    h[0] = f & 65535;
    for (let i = 1; i < 8; i++) {
      f = (h[i] + pad[i] | 0) + (f >>> 16) | 0;
      h[i] = f & 65535;
    }
    clean(g);
  }
  update(data) {
    aexists(this);
    abytes(data);
    data = copyBytes(data);
    const { buffer, blockLen } = this;
    const len = data.length;
    for (let pos = 0; pos < len; ) {
      const take = Math.min(blockLen - this.pos, len - pos);
      if (take === blockLen) {
        for (; blockLen <= len - pos; pos += blockLen)
          this.process(data, pos);
        continue;
      }
      buffer.set(data.subarray(pos, pos + take), this.pos);
      this.pos += take;
      pos += take;
      if (this.pos === blockLen) {
        this.process(buffer, 0, false);
        this.pos = 0;
      }
    }
    return this;
  }
  destroy() {
    clean(this.h, this.r, this.buffer, this.pad);
  }
  digestInto(out) {
    aexists(this);
    aoutput(out, this);
    this.finished = true;
    const { buffer, h } = this;
    let { pos } = this;
    if (pos) {
      buffer[pos++] = 1;
      for (; pos < 16; pos++)
        buffer[pos] = 0;
      this.process(buffer, 0, true);
    }
    this.finalize();
    let opos = 0;
    for (let i = 0; i < 8; i++) {
      out[opos++] = h[i] >>> 0;
      out[opos++] = h[i] >>> 8;
    }
    return out;
  }
  digest() {
    const { buffer, outputLen } = this;
    this.digestInto(buffer);
    const res = buffer.slice(0, outputLen);
    this.destroy();
    return res;
  }
};
function wrapConstructorWithKey(hashCons) {
  const hashC = (msg, key) => hashCons(key).update(msg).digest();
  const tmp = hashCons(new Uint8Array(32));
  hashC.outputLen = tmp.outputLen;
  hashC.blockLen = tmp.blockLen;
  hashC.create = (key) => hashCons(key);
  return hashC;
}
var poly1305 = /* @__PURE__ */ (() => wrapConstructorWithKey((key) => new Poly1305(key)))();

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/chacha20poly1305/esm/src/chacha/chacha.js
function chachaCore(s, k, n, out, cnt, rounds = 20) {
  const y00 = s[0], y01 = s[1], y02 = s[2], y03 = s[3], y04 = k[0], y05 = k[1], y06 = k[2], y07 = k[3], y08 = k[4], y09 = k[5], y10 = k[6], y11 = k[7], y12 = cnt, y13 = n[0], y14 = n[1], y15 = n[2];
  let x00 = y00, x01 = y01, x02 = y02, x03 = y03, x04 = y04, x05 = y05, x06 = y06, x07 = y07, x08 = y08, x09 = y09, x10 = y10, x11 = y11, x12 = y12, x13 = y13, x14 = y14, x15 = y15;
  for (let r = 0; r < rounds; r += 2) {
    x00 = x00 + x04 | 0;
    x12 = rotl(x12 ^ x00, 16);
    x08 = x08 + x12 | 0;
    x04 = rotl(x04 ^ x08, 12);
    x00 = x00 + x04 | 0;
    x12 = rotl(x12 ^ x00, 8);
    x08 = x08 + x12 | 0;
    x04 = rotl(x04 ^ x08, 7);
    x01 = x01 + x05 | 0;
    x13 = rotl(x13 ^ x01, 16);
    x09 = x09 + x13 | 0;
    x05 = rotl(x05 ^ x09, 12);
    x01 = x01 + x05 | 0;
    x13 = rotl(x13 ^ x01, 8);
    x09 = x09 + x13 | 0;
    x05 = rotl(x05 ^ x09, 7);
    x02 = x02 + x06 | 0;
    x14 = rotl(x14 ^ x02, 16);
    x10 = x10 + x14 | 0;
    x06 = rotl(x06 ^ x10, 12);
    x02 = x02 + x06 | 0;
    x14 = rotl(x14 ^ x02, 8);
    x10 = x10 + x14 | 0;
    x06 = rotl(x06 ^ x10, 7);
    x03 = x03 + x07 | 0;
    x15 = rotl(x15 ^ x03, 16);
    x11 = x11 + x15 | 0;
    x07 = rotl(x07 ^ x11, 12);
    x03 = x03 + x07 | 0;
    x15 = rotl(x15 ^ x03, 8);
    x11 = x11 + x15 | 0;
    x07 = rotl(x07 ^ x11, 7);
    x00 = x00 + x05 | 0;
    x15 = rotl(x15 ^ x00, 16);
    x10 = x10 + x15 | 0;
    x05 = rotl(x05 ^ x10, 12);
    x00 = x00 + x05 | 0;
    x15 = rotl(x15 ^ x00, 8);
    x10 = x10 + x15 | 0;
    x05 = rotl(x05 ^ x10, 7);
    x01 = x01 + x06 | 0;
    x12 = rotl(x12 ^ x01, 16);
    x11 = x11 + x12 | 0;
    x06 = rotl(x06 ^ x11, 12);
    x01 = x01 + x06 | 0;
    x12 = rotl(x12 ^ x01, 8);
    x11 = x11 + x12 | 0;
    x06 = rotl(x06 ^ x11, 7);
    x02 = x02 + x07 | 0;
    x13 = rotl(x13 ^ x02, 16);
    x08 = x08 + x13 | 0;
    x07 = rotl(x07 ^ x08, 12);
    x02 = x02 + x07 | 0;
    x13 = rotl(x13 ^ x02, 8);
    x08 = x08 + x13 | 0;
    x07 = rotl(x07 ^ x08, 7);
    x03 = x03 + x04 | 0;
    x14 = rotl(x14 ^ x03, 16);
    x09 = x09 + x14 | 0;
    x04 = rotl(x04 ^ x09, 12);
    x03 = x03 + x04 | 0;
    x14 = rotl(x14 ^ x03, 8);
    x09 = x09 + x14 | 0;
    x04 = rotl(x04 ^ x09, 7);
  }
  let oi = 0;
  out[oi++] = y00 + x00 | 0;
  out[oi++] = y01 + x01 | 0;
  out[oi++] = y02 + x02 | 0;
  out[oi++] = y03 + x03 | 0;
  out[oi++] = y04 + x04 | 0;
  out[oi++] = y05 + x05 | 0;
  out[oi++] = y06 + x06 | 0;
  out[oi++] = y07 + x07 | 0;
  out[oi++] = y08 + x08 | 0;
  out[oi++] = y09 + x09 | 0;
  out[oi++] = y10 + x10 | 0;
  out[oi++] = y11 + x11 | 0;
  out[oi++] = y12 + x12 | 0;
  out[oi++] = y13 + x13 | 0;
  out[oi++] = y14 + x14 | 0;
  out[oi++] = y15 + x15 | 0;
}
var chacha20 = /* @__PURE__ */ createCipher(chachaCore, {
  counterRight: false,
  counterLength: 4,
  allowShortKeys: false
});
var ZEROS16 = /* @__PURE__ */ new Uint8Array(16);
var updatePadded = (h, msg) => {
  h.update(msg);
  const leftover = msg.length % 16;
  if (leftover)
    h.update(ZEROS16.subarray(leftover));
};
var ZEROS32 = /* @__PURE__ */ new Uint8Array(32);
function computeTag(fn, key, nonce, ciphertext, AAD) {
  if (AAD !== void 0)
    abytes(AAD, void 0, "AAD");
  const authKey = fn(key, nonce, ZEROS32);
  const lengths = u64Lengths(ciphertext.length, AAD ? AAD.length : 0, true);
  const h = poly1305.create(authKey);
  if (AAD)
    updatePadded(h, AAD);
  updatePadded(h, ciphertext);
  h.update(lengths);
  const res = h.digest();
  clean(authKey, lengths);
  return res;
}
var _poly1305_aead = (xorStream) => (key, nonce, AAD) => {
  const tagLength = 16;
  return {
    encrypt(plaintext, output) {
      const plength = plaintext.length;
      output = getOutput(plength + tagLength, output, false);
      output.set(plaintext);
      const oPlain = output.subarray(0, -tagLength);
      xorStream(key, nonce, oPlain, oPlain, 1);
      const tag = computeTag(xorStream, key, nonce, oPlain, AAD);
      output.set(tag, plength);
      clean(tag);
      return output;
    },
    decrypt(ciphertext, output) {
      output = getOutput(ciphertext.length - tagLength, output, false);
      const data = ciphertext.subarray(0, -tagLength);
      const passedTag = ciphertext.subarray(-tagLength);
      const tag = computeTag(xorStream, key, nonce, data, AAD);
      if (!equalBytes(passedTag, tag))
        throw new Error("invalid tag");
      output.set(ciphertext.subarray(0, -tagLength));
      xorStream(key, nonce, output, output, 1);
      clean(tag);
      return output;
    }
  };
};
var chacha20poly1305 = /* @__PURE__ */ wrapCipher({ blockSize: 64, nonceLength: 12, tagLength: 16 }, _poly1305_aead(chacha20));

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/chacha20poly1305/esm/src/chacha20Poly1305.js
var Chacha20Poly1305Context = class {
  constructor(key) {
    Object.defineProperty(this, "_key", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    this._key = new Uint8Array(toArrayBuffer(key));
  }
  async seal(iv, data, aad) {
    return await this._seal(toArrayBuffer(iv), toArrayBuffer(data), toArrayBuffer(aad));
  }
  async open(iv, data, aad) {
    return await this._open(toArrayBuffer(iv), toArrayBuffer(data), toArrayBuffer(aad));
  }
  _seal(iv, data, aad) {
    return new Promise((resolve) => {
      const ret = chacha20poly1305(this._key, new Uint8Array(iv), new Uint8Array(aad)).encrypt(new Uint8Array(data));
      resolve(ret.buffer);
    });
  }
  _open(iv, data, aad) {
    return new Promise((resolve) => {
      const ret = chacha20poly1305(this._key, new Uint8Array(iv), new Uint8Array(aad)).decrypt(new Uint8Array(data));
      resolve(ret.buffer);
    });
  }
};
var Chacha20Poly1305 = class {
  constructor() {
    Object.defineProperty(this, "id", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: AeadId.Chacha20Poly1305
    });
    Object.defineProperty(this, "keySize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 32
    });
    Object.defineProperty(this, "nonceSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 12
    });
    Object.defineProperty(this, "tagSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 16
    });
  }
  createEncryptionContext(key) {
    return new Chacha20Poly1305Context(key);
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/utils/emitNotSupported.js
function emitNotSupported() {
  return new Promise((_resolve, reject) => {
    reject(new NotSupportedError("Not supported"));
  });
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/exporterContext.js
var LABEL_SEC = new Uint8Array([115, 101, 99]);
var ExporterContextImpl = class {
  constructor(api, kdf, exporterSecret) {
    Object.defineProperty(this, "_api", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "exporterSecret", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_kdf", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    this._api = api;
    this._kdf = kdf;
    this.exporterSecret = exporterSecret;
  }
  async seal(_data, _aad) {
    return await emitNotSupported();
  }
  async open(_data, _aad) {
    return await emitNotSupported();
  }
  async export(exporterContext, len) {
    const rawExporterContext = toArrayBuffer(exporterContext);
    if (rawExporterContext.byteLength > INPUT_LENGTH_LIMIT) {
      throw new InvalidParamError("Too long exporter context");
    }
    try {
      return await this._kdf.labeledExpand(this.exporterSecret, LABEL_SEC, new Uint8Array(rawExporterContext), len);
    } catch (e) {
      throw new ExportError(e);
    }
  }
};
var RecipientExporterContextImpl = class extends ExporterContextImpl {
};
var SenderExporterContextImpl = class extends ExporterContextImpl {
  constructor(api, kdf, exporterSecret, enc) {
    super(api, kdf, exporterSecret);
    Object.defineProperty(this, "enc", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    this.enc = enc;
    return;
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/encryptionContext.js
var EncryptionContextImpl = class extends ExporterContextImpl {
  constructor(api, kdf, params) {
    super(api, kdf, params.exporterSecret);
    Object.defineProperty(this, "_aead", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_nK", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_nN", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_nT", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_ctx", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    if (params.key === void 0 || params.baseNonce === void 0 || params.seq === void 0) {
      throw new Error("Required parameters are missing");
    }
    this._aead = params.aead;
    this._nK = this._aead.keySize;
    this._nN = this._aead.nonceSize;
    this._nT = this._aead.tagSize;
    const key = this._aead.createEncryptionContext(params.key);
    this._ctx = {
      key,
      baseNonce: params.baseNonce,
      seq: params.seq
    };
  }
  computeNonce(k) {
    const seqBytes = i2Osp(k.seq, k.baseNonce.byteLength);
    return xor(k.baseNonce, seqBytes).buffer;
  }
  incrementSeq(k) {
    if (k.seq > Number.MAX_SAFE_INTEGER) {
      throw new MessageLimitReachedError("Message limit reached");
    }
    k.seq += 1;
    return;
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/mutex.js
var __classPrivateFieldGet = function(receiver, state, kind, f) {
  if (kind === "a" && !f) throw new TypeError("Private accessor was defined without a getter");
  if (typeof state === "function" ? receiver !== state || !f : !state.has(receiver)) throw new TypeError("Cannot read private member from an object whose class did not declare it");
  return kind === "m" ? f : kind === "a" ? f.call(receiver) : f ? f.value : state.get(receiver);
};
var __classPrivateFieldSet = function(receiver, state, value, kind, f) {
  if (kind === "m") throw new TypeError("Private method is not writable");
  if (kind === "a" && !f) throw new TypeError("Private accessor was defined without a setter");
  if (typeof state === "function" ? receiver !== state || !f : !state.has(receiver)) throw new TypeError("Cannot write private member to an object whose class did not declare it");
  return kind === "a" ? f.call(receiver, value) : f ? f.value = value : state.set(receiver, value), value;
};
var _Mutex_locked;
var Mutex = class {
  constructor() {
    _Mutex_locked.set(this, Promise.resolve());
  }
  async lock() {
    let releaseLock;
    const nextLock = new Promise((resolve) => {
      releaseLock = resolve;
    });
    const previousLock = __classPrivateFieldGet(this, _Mutex_locked, "f");
    __classPrivateFieldSet(this, _Mutex_locked, nextLock, "f");
    await previousLock;
    return releaseLock;
  }
};
_Mutex_locked = /* @__PURE__ */ new WeakMap();

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/recipientContext.js
var __classPrivateFieldGet2 = function(receiver, state, kind, f) {
  if (kind === "a" && !f) throw new TypeError("Private accessor was defined without a getter");
  if (typeof state === "function" ? receiver !== state || !f : !state.has(receiver)) throw new TypeError("Cannot read private member from an object whose class did not declare it");
  return kind === "m" ? f : kind === "a" ? f.call(receiver) : f ? f.value : state.get(receiver);
};
var __classPrivateFieldSet2 = function(receiver, state, value, kind, f) {
  if (kind === "m") throw new TypeError("Private method is not writable");
  if (kind === "a" && !f) throw new TypeError("Private accessor was defined without a setter");
  if (typeof state === "function" ? receiver !== state || !f : !state.has(receiver)) throw new TypeError("Cannot write private member to an object whose class did not declare it");
  return kind === "a" ? f.call(receiver, value) : f ? f.value = value : state.set(receiver, value), value;
};
var _RecipientContextImpl_mutex;
var RecipientContextImpl = class extends EncryptionContextImpl {
  constructor() {
    super(...arguments);
    _RecipientContextImpl_mutex.set(this, void 0);
  }
  async open(data, aad = EMPTY.buffer) {
    __classPrivateFieldSet2(this, _RecipientContextImpl_mutex, __classPrivateFieldGet2(this, _RecipientContextImpl_mutex, "f") ?? new Mutex(), "f");
    const release = await __classPrivateFieldGet2(this, _RecipientContextImpl_mutex, "f").lock();
    let pt;
    try {
      pt = await this._ctx.key.open(this.computeNonce(this._ctx), toArrayBuffer(data), toArrayBuffer(aad));
    } catch (e) {
      throw new OpenError(e);
    } finally {
      release();
    }
    this.incrementSeq(this._ctx);
    return pt;
  }
};
_RecipientContextImpl_mutex = /* @__PURE__ */ new WeakMap();

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/senderContext.js
var __classPrivateFieldGet3 = function(receiver, state, kind, f) {
  if (kind === "a" && !f) throw new TypeError("Private accessor was defined without a getter");
  if (typeof state === "function" ? receiver !== state || !f : !state.has(receiver)) throw new TypeError("Cannot read private member from an object whose class did not declare it");
  return kind === "m" ? f : kind === "a" ? f.call(receiver) : f ? f.value : state.get(receiver);
};
var __classPrivateFieldSet3 = function(receiver, state, value, kind, f) {
  if (kind === "m") throw new TypeError("Private method is not writable");
  if (kind === "a" && !f) throw new TypeError("Private accessor was defined without a setter");
  if (typeof state === "function" ? receiver !== state || !f : !state.has(receiver)) throw new TypeError("Cannot write private member to an object whose class did not declare it");
  return kind === "a" ? f.call(receiver, value) : f ? f.value = value : state.set(receiver, value), value;
};
var _SenderContextImpl_mutex;
var SenderContextImpl = class extends EncryptionContextImpl {
  constructor(api, kdf, params, enc) {
    super(api, kdf, params);
    Object.defineProperty(this, "enc", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    _SenderContextImpl_mutex.set(this, void 0);
    this.enc = enc;
  }
  async seal(data, aad = EMPTY.buffer) {
    __classPrivateFieldSet3(this, _SenderContextImpl_mutex, __classPrivateFieldGet3(this, _SenderContextImpl_mutex, "f") ?? new Mutex(), "f");
    const release = await __classPrivateFieldGet3(this, _SenderContextImpl_mutex, "f").lock();
    let ct;
    try {
      ct = await this._ctx.key.seal(this.computeNonce(this._ctx), toArrayBuffer(data), toArrayBuffer(aad));
    } catch (e) {
      throw new SealError(e);
    } finally {
      release();
    }
    this.incrementSeq(this._ctx);
    return ct;
  }
};
_SenderContextImpl_mutex = /* @__PURE__ */ new WeakMap();

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/cipherSuiteNative.js
var LABEL_BASE_NONCE = new Uint8Array([
  98,
  97,
  115,
  101,
  95,
  110,
  111,
  110,
  99,
  101
]);
var LABEL_EXP = new Uint8Array([101, 120, 112]);
var LABEL_INFO_HASH = new Uint8Array([
  105,
  110,
  102,
  111,
  95,
  104,
  97,
  115,
  104
]);
var LABEL_KEY = new Uint8Array([107, 101, 121]);
var LABEL_PSK_ID_HASH = new Uint8Array([
  112,
  115,
  107,
  95,
  105,
  100,
  95,
  104,
  97,
  115,
  104
]);
var LABEL_SECRET = new Uint8Array([115, 101, 99, 114, 101, 116]);
var SUITE_ID_HEADER_HPKE = new Uint8Array([
  72,
  80,
  75,
  69,
  0,
  0,
  0,
  0,
  0,
  0
]);
var CipherSuiteNative = class extends NativeAlgorithm {
  /**
   * @param params A set of parameters for building a cipher suite.
   *
   * If the error occurred, throws {@link InvalidParamError}.
   *
   * @throws {@link InvalidParamError}
   */
  constructor(params) {
    super();
    Object.defineProperty(this, "_kem", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_kdf", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_aead", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    Object.defineProperty(this, "_suiteId", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: void 0
    });
    if (typeof params.kem === "number") {
      throw new InvalidParamError("KemId cannot be used");
    }
    this._kem = params.kem;
    if (typeof params.kdf === "number") {
      throw new InvalidParamError("KdfId cannot be used");
    }
    this._kdf = params.kdf;
    if (typeof params.aead === "number") {
      throw new InvalidParamError("AeadId cannot be used");
    }
    this._aead = params.aead;
    this._suiteId = new Uint8Array(SUITE_ID_HEADER_HPKE);
    this._suiteId.set(i2Osp(this._kem.id, 2), 4);
    this._suiteId.set(i2Osp(this._kdf.id, 2), 6);
    this._suiteId.set(i2Osp(this._aead.id, 2), 8);
    this._kdf.init(this._suiteId);
  }
  /**
   * Gets the KEM context of the ciphersuite.
   */
  get kem() {
    return this._kem;
  }
  /**
   * Gets the KDF context of the ciphersuite.
   */
  get kdf() {
    return this._kdf;
  }
  /**
   * Gets the AEAD context of the ciphersuite.
   */
  get aead() {
    return this._aead;
  }
  /**
   * Creates an encryption context for a sender.
   *
   * If the error occurred, throws {@link DecapError} | {@link ValidationError}.
   *
   * @param params A set of parameters for the sender encryption context.
   * @returns A sender encryption context.
   * @throws {@link EncapError}, {@link ValidationError}
   */
  async createSenderContext(params) {
    this._validateInputLength(params);
    await this._setup();
    const dh = await this._kem.encap(params);
    let mode;
    if (params.psk !== void 0) {
      mode = params.senderKey !== void 0 ? Mode.AuthPsk : Mode.Psk;
    } else {
      mode = params.senderKey !== void 0 ? Mode.Auth : Mode.Base;
    }
    return await this._keyScheduleS(mode, dh.sharedSecret, dh.enc, params);
  }
  /**
   * Creates an encryption context for a recipient.
   *
   * If the error occurred, throws {@link DecapError}
   * | {@link DeserializeError} | {@link ValidationError}.
   *
   * @param params A set of parameters for the recipient encryption context.
   * @returns A recipient encryption context.
   * @throws {@link DecapError}, {@link DeserializeError}, {@link ValidationError}
   */
  async createRecipientContext(params) {
    this._validateInputLength(params);
    await this._setup();
    const sharedSecret = await this._kem.decap(params);
    let mode;
    if (params.psk !== void 0) {
      mode = params.senderPublicKey !== void 0 ? Mode.AuthPsk : Mode.Psk;
    } else {
      mode = params.senderPublicKey !== void 0 ? Mode.Auth : Mode.Base;
    }
    return await this._keyScheduleR(mode, sharedSecret, params);
  }
  /**
   * Encrypts a message to a recipient.
   *
   * If the error occurred, throws `EncapError` | `MessageLimitReachedError` | `SealError` | `ValidationError`.
   *
   * @param params A set of parameters for building a sender encryption context.
   * @param pt A plain text as bytes to be encrypted.
   * @param aad Additional authenticated data as bytes fed by an application.
   * @returns A cipher text and an encapsulated key as bytes.
   * @throws {@link EncapError}, {@link MessageLimitReachedError}, {@link SealError}, {@link ValidationError}
   */
  async seal(params, pt, aad = EMPTY.buffer) {
    const ctx = await this.createSenderContext(params);
    return {
      ct: await ctx.seal(pt, aad),
      enc: ctx.enc
    };
  }
  /**
   * Decrypts a message from a sender.
   *
   * If the error occurred, throws `DecapError` | `DeserializeError` | `OpenError` | `ValidationError`.
   *
   * @param params A set of parameters for building a recipient encryption context.
   * @param ct An encrypted text as bytes to be decrypted.
   * @param aad Additional authenticated data as bytes fed by an application.
   * @returns A decrypted plain text as bytes.
   * @throws {@link DecapError}, {@link DeserializeError}, {@link OpenError}, {@link ValidationError}
   */
  async open(params, ct, aad = EMPTY.buffer) {
    const ctx = await this.createRecipientContext(params);
    return await ctx.open(ct, aad);
  }
  // private verifyPskInputs(mode: Mode, params: KeyScheduleParams) {
  //   const gotPsk = (params.psk !== undefined);
  //   const gotPskId = (params.psk !== undefined && params.psk.id.byteLength > 0);
  //   if (gotPsk !== gotPskId) {
  //     throw new Error('Inconsistent PSK inputs');
  //   }
  //   if (gotPsk && (mode === Mode.Base || mode === Mode.Auth)) {
  //     throw new Error('PSK input provided when not needed');
  //   }
  //   if (!gotPsk && (mode === Mode.Psk || mode === Mode.AuthPsk)) {
  //     throw new Error('Missing required PSK input');
  //   }
  //   return;
  // }
  async _keySchedule(mode, sharedSecret, params) {
    const pskId = params.psk === void 0 ? EMPTY : toUint8Array(params.psk.id);
    const pskIdHash = await this._kdf.labeledExtract(EMPTY, LABEL_PSK_ID_HASH, pskId);
    const info = params.info === void 0 ? EMPTY : toUint8Array(params.info);
    const infoHash = await this._kdf.labeledExtract(EMPTY, LABEL_INFO_HASH, info);
    const keyScheduleContext = new Uint8Array(1 + pskIdHash.byteLength + infoHash.byteLength);
    keyScheduleContext.set(new Uint8Array([mode]), 0);
    keyScheduleContext.set(new Uint8Array(pskIdHash), 1);
    keyScheduleContext.set(new Uint8Array(infoHash), 1 + pskIdHash.byteLength);
    const psk = params.psk === void 0 ? EMPTY : toUint8Array(params.psk.key);
    const ikm = this._kdf.buildLabeledIkm(LABEL_SECRET, psk);
    const exporterSecretInfo = this._kdf.buildLabeledInfo(LABEL_EXP, keyScheduleContext, this._kdf.hashSize);
    const exporterSecret = await this._kdf.extractAndExpand(sharedSecret, ikm, exporterSecretInfo, this._kdf.hashSize);
    if (this._aead.id === AeadId.ExportOnly) {
      return { aead: this._aead, exporterSecret };
    }
    const keyInfo = this._kdf.buildLabeledInfo(LABEL_KEY, keyScheduleContext, this._aead.keySize);
    const key = await this._kdf.extractAndExpand(sharedSecret, ikm, keyInfo, this._aead.keySize);
    const baseNonceInfo = this._kdf.buildLabeledInfo(LABEL_BASE_NONCE, keyScheduleContext, this._aead.nonceSize);
    const baseNonce = await this._kdf.extractAndExpand(sharedSecret, ikm, baseNonceInfo, this._aead.nonceSize);
    return {
      aead: this._aead,
      exporterSecret,
      key,
      baseNonce: new Uint8Array(baseNonce),
      seq: 0
    };
  }
  async _keyScheduleS(mode, sharedSecret, enc, params) {
    const res = await this._keySchedule(mode, sharedSecret, params);
    if (res.key === void 0) {
      return new SenderExporterContextImpl(this._api, this._kdf, res.exporterSecret, enc);
    }
    return new SenderContextImpl(this._api, this._kdf, res, enc);
  }
  async _keyScheduleR(mode, sharedSecret, params) {
    const res = await this._keySchedule(mode, sharedSecret, params);
    if (res.key === void 0) {
      return new RecipientExporterContextImpl(this._api, this._kdf, res.exporterSecret);
    }
    return new RecipientContextImpl(this._api, this._kdf, res);
  }
  _validateInputLength(params) {
    if (params.info !== void 0 && params.info.byteLength > INFO_LENGTH_LIMIT) {
      throw new InvalidParamError("Too long info");
    }
    if (params.psk !== void 0) {
      if (params.psk.key.byteLength < MINIMUM_PSK_LENGTH) {
        throw new InvalidParamError(`PSK must have at least ${MINIMUM_PSK_LENGTH} bytes`);
      }
      if (params.psk.key.byteLength > INPUT_LENGTH_LIMIT) {
        throw new InvalidParamError("Too long psk.key");
      }
      if (params.psk.id.byteLength > INPUT_LENGTH_LIMIT) {
        throw new InvalidParamError("Too long psk.id");
      }
    }
    return;
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/native.js
var CipherSuite = class extends CipherSuiteNative {
};
var HkdfSha256 = class extends HkdfSha256Native {
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/kems/dhkemPrimitives/x25519.js
var PKCS8_ALG_ID_X25519 = new Uint8Array([
  48,
  46,
  2,
  1,
  0,
  48,
  5,
  6,
  3,
  43,
  101,
  110,
  4,
  34,
  4,
  32
]);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/core/esm/src/kems/dhkemPrimitives/x448.js
var PKCS8_ALG_ID_X448 = new Uint8Array([
  48,
  70,
  2,
  1,
  0,
  48,
  5,
  6,
  3,
  43,
  101,
  111,
  4,
  58,
  4,
  56
]);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/dhkem-x25519/esm/src/primitives/x25519.js
var _1n2 = 1n;
var _2n2 = 2n;
var _3n = 3n;
var _5n = 5n;
var ed25519_CURVE_p = 0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffedn;
function ed25519_pow_2_252_3(x) {
  const _10n = 10n;
  const _20n = 20n;
  const _40n = 40n;
  const _80n = 80n;
  const P = ed25519_CURVE_p;
  const x2 = x * x % P;
  const b2 = x2 * x % P;
  const b4 = pow2(b2, _2n2, P) * b2 % P;
  const b5 = pow2(b4, _1n2, P) * x % P;
  const b10 = pow2(b5, _5n, P) * b5 % P;
  const b20 = pow2(b10, _10n, P) * b10 % P;
  const b40 = pow2(b20, _20n, P) * b20 % P;
  const b80 = pow2(b40, _40n, P) * b40 % P;
  const b160 = pow2(b80, _80n, P) * b80 % P;
  const b240 = pow2(b160, _80n, P) * b80 % P;
  const b250 = pow2(b240, _10n, P) * b10 % P;
  const pow_p_5_8 = pow2(b250, _2n2, P) * x % P;
  return { pow_p_5_8, b2 };
}
function adjustScalarBytes(bytes) {
  bytes[0] &= 248;
  bytes[31] &= 127;
  bytes[31] |= 64;
  return bytes;
}
var x25519 = /* @__PURE__ */ (() => {
  const P = ed25519_CURVE_p;
  return montgomery({
    P,
    type: "x25519",
    powPminus2: (x) => {
      const { pow_p_5_8, b2 } = ed25519_pow_2_252_3(x);
      return mod(pow2(pow_p_5_8, _3n, P) * b2, P);
    },
    adjustScalarBytes
  });
})();

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/dhkem-x25519/esm/src/hkdfSha256.js
var HkdfSha2562 = class extends HkdfSha256Native {
  async extract(salt, ikm) {
    await this._setup();
    const saltBuf = salt.byteLength === 0 ? new ArrayBuffer(this.hashSize) : toArrayBuffer(salt);
    const ikmBuf = toArrayBuffer(ikm);
    if (saltBuf.byteLength !== this.hashSize) {
      return hmac(sha256, new Uint8Array(saltBuf), new Uint8Array(ikmBuf)).buffer;
    }
    const key = await this._api.importKey("raw", saltBuf, this.algHash, false, [
      "sign"
    ]);
    return await this._api.sign("HMAC", key, ikmBuf);
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@hpke/dhkem-x25519/esm/src/dhkemX25519.js
var X255192 = class extends XCurveDhkemPrimitives {
  constructor(hkdf2) {
    super("X25519", 32, x25519, hkdf2);
  }
  derive(sk, pk) {
    try {
      return Promise.resolve(this._curve.getSharedSecret(sk, pk));
    } catch (e) {
      return Promise.reject(new SerializeError(e));
    }
  }
};
var DhkemX25519HkdfSha2562 = class extends Dhkem {
  constructor() {
    const kdf = new HkdfSha2562();
    super(KemId.DhkemX25519HkdfSha256, new X255192(kdf), kdf);
    Object.defineProperty(this, "id", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: KemId.DhkemX25519HkdfSha256
    });
    Object.defineProperty(this, "secretSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 32
    });
    Object.defineProperty(this, "encSize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 32
    });
    Object.defineProperty(this, "publicKeySize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 32
    });
    Object.defineProperty(this, "privateKeySize", {
      enumerable: true,
      configurable: true,
      writable: true,
      value: 32
    });
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/ciphers/utils.js
function isBytes2(a) {
  return a instanceof Uint8Array || ArrayBuffer.isView(a) && a.constructor.name === "Uint8Array" && "BYTES_PER_ELEMENT" in a && a.BYTES_PER_ELEMENT === 1;
}
var atitle = (title) => title ? `"${title}" ` : "";
function abool2(value, title = "") {
  if (typeof value !== "boolean")
    throw new TypeError(atitle(title) + "expected boolean, got type=" + typeof value);
  return value;
}
function anumber2(n, title = "") {
  if (typeof n !== "number")
    throw new TypeError(atitle(title) + "expected number, got " + typeof n);
  if (!Number.isSafeInteger(n) || n < 0)
    throw new RangeError(atitle(title) + "expected integer >= 0, got " + n);
  return n;
}
function abytes2(value, length, title = "") {
  if (isBytes2(value) && (length === void 0 || value.length === length))
    return value;
  if (length !== void 0)
    anumber2(length, "length");
  const bytes = isBytes2(value);
  const ofLen = length !== void 0 ? ` of length ${length}` : "";
  const got = bytes ? `length=${value.length}` : `type=${typeof value}`;
  const message = atitle(title) + "expected Uint8Array" + ofLen + ", got " + got;
  if (!bytes)
    throw new TypeError(message);
  throw new RangeError(message);
}
var aobject = (value, label) => {
  if (value === null || typeof value !== "object" || Array.isArray(value))
    throw new TypeError(label === "object" ? "expected valid options object" : `"${label}" expected object, got type=${typeof value}`);
};
function aexists2(instance, checkFinished = true) {
  if (instance.destroyed)
    throw new Error("hash was destroyed");
  if (checkFinished && instance.finished)
    throw new Error("digest() was already called");
}
function aoutput2(out, instance) {
  abytes2(out, void 0, "output");
  const min = instance.outputLen;
  if (!(out.length >= min)) {
    throw new RangeError('"output" expected length >= ' + min);
  }
}
function u322(arr) {
  return new Uint32Array(arr.buffer, arr.byteOffset, Math.floor(arr.byteLength / 4));
}
function clean2(...arrays) {
  for (let i = 0; i < arrays.length; i++) {
    arrays[i].fill(0);
  }
}
function createView2(arr) {
  return new DataView(arr.buffer, arr.byteOffset, arr.byteLength);
}
var isLE2 = /* @__PURE__ */ (() => new Uint8Array(new Uint32Array([287454020]).buffer)[0] === 68)();
function byteSwap(word) {
  return word << 24 & 4278190080 | word << 8 & 16711680 | word >>> 8 & 65280 | word >>> 24 & 255;
}
function byteSwap32(arr) {
  for (let i = 0; i < arr.length; i++) {
    arr[i] = byteSwap(arr[i]);
  }
  return arr;
}
var swap32IfBE2 = isLE2 ? (u) => u : byteSwap32;
function overlapBytes(a, b) {
  if (!a.byteLength || !b.byteLength)
    return false;
  return a.buffer === b.buffer && // best we can do, may fail with an obscure Proxy
  a.byteOffset < b.byteOffset + b.byteLength && // a starts before b end
  b.byteOffset < a.byteOffset + a.byteLength;
}
function complexOverlapBytes(input, output) {
  if (overlapBytes(input, output) && input.byteOffset < output.byteOffset)
    throw new Error("complex overlap of input and output is not supported");
}
function checkOpts2(defaults, opts) {
  aobject(defaults, "defaults");
  aobject(opts, "opts");
  const merged = Object.assign(defaults, opts);
  return merged;
}
function equalBytes2(a, b) {
  a = abytes2(a);
  b = abytes2(b);
  if (a.length !== b.length)
    return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++)
    diff |= a[i] ^ b[i];
  return diff === 0;
}
function wrapMacConstructor(keyLen, macCons, fromMsg) {
  const mac = macCons;
  const getArgs = fromMsg || (() => []);
  const macC = (msg, key) => mac(key, ...getArgs(msg)).update(msg).digest();
  const tmp = mac(new Uint8Array(keyLen), ...getArgs(new Uint8Array(0)));
  macC.outputLen = tmp.outputLen;
  macC.blockLen = tmp.blockLen;
  macC.create = (key, ...args) => mac(key, ...args);
  return macC;
}
var wrapCipher2 = /* @__NO_SIDE_EFFECTS__ */ (params, constructor) => {
  function wrappedCipher(key, ...args) {
    abytes2(key, void 0, "key");
    if (params.nonceLength !== void 0) {
      const nonce = args[0];
      abytes2(nonce, params.varSizeNonce ? void 0 : params.nonceLength, "nonce");
    }
    const tagl = params.tagLength;
    const aadStart = params.nonceLength !== void 0 ? 1 : 0;
    if (!params.withAAD) {
      for (let i = aadStart; i < args.length; i++)
        if (isBytes2(args[i]))
          throw new Error("AAD not supported");
    }
    if (params.withAAD && args[aadStart] !== void 0)
      abytes2(args[aadStart], void 0, "AAD");
    const cipher = constructor(key, ...args);
    const checkOutput = (fnLength, output) => {
      if (output !== void 0) {
        if (fnLength !== 2)
          throw new Error("cipher output not supported");
        abytes2(output, void 0, "output");
      }
    };
    let called = false;
    const wrCipher = {
      encrypt(data, output) {
        if (called)
          throw new Error("cannot encrypt() twice with same key + nonce");
        called = true;
        abytes2(data, void 0, "data");
        checkOutput(cipher.encrypt.length, output);
        return cipher.encrypt(data, output);
      },
      decrypt(data, output) {
        abytes2(data, void 0, "data");
        if (tagl && data.length < tagl)
          throw new Error('"ciphertext" expected length >= tagLength=' + tagl);
        checkOutput(cipher.decrypt.length, output);
        return cipher.decrypt(data, output);
      }
    };
    return wrCipher;
  }
  Object.assign(wrappedCipher, params);
  return wrappedCipher;
};
function getOutput2(expectedLength, out, onlyAligned = true) {
  if (out === void 0)
    return new Uint8Array(expectedLength);
  abytes2(out, expectedLength, "output");
  if (onlyAligned && !isAligned323(out))
    throw new Error("invalid output, must be aligned");
  return out;
}
function u64Lengths2(dataLength, aadLength, isLE3) {
  anumber2(dataLength);
  anumber2(aadLength);
  abool2(isLE3);
  const num = new Uint8Array(16);
  const view = createView2(num);
  view.setBigUint64(0, BigInt(aadLength), isLE3);
  view.setBigUint64(8, BigInt(dataLength), isLE3);
  return num;
}
function isAligned323(bytes) {
  return bytes.byteOffset % 4 === 0;
}
function copyBytes2(bytes) {
  return Uint8Array.from(abytes2(bytes));
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/ciphers/_arx.js
var encodeStr = (str) => Uint8Array.from(str.split(""), (c) => c.charCodeAt(0));
var sigma16_322 = /* @__PURE__ */ (() => swap32IfBE2(u322(encodeStr("expand 16-byte k"))))();
var sigma32_322 = /* @__PURE__ */ (() => swap32IfBE2(u322(encodeStr("expand 32-byte k"))))();
function rotl2(a, b) {
  return a << b | a >>> 32 - b;
}
var BLOCK_LEN2 = 64;
var BLOCK_LEN322 = 16;
var MAX_COUNTER2 = /* @__PURE__ */ (() => 2 ** 32 - 1)();
var U32_EMPTY2 = /* @__PURE__ */ Uint32Array.of();
function runCipher2(core, sigma, key, nonce, data, output, counter, rounds) {
  const len = data.length;
  const block = new Uint8Array(BLOCK_LEN2);
  const b32 = u322(block);
  const isAligned = isLE2 && isAligned323(data) && isAligned323(output);
  const d32 = isAligned ? u322(data) : U32_EMPTY2;
  const o32 = isAligned ? u322(output) : U32_EMPTY2;
  if (!isLE2) {
    for (let pos = 0; pos < len; counter++) {
      core(sigma, key, nonce, b32, counter, rounds);
      swap32IfBE2(b32);
      if (counter >= MAX_COUNTER2)
        throw new Error("arx: counter overflow");
      const take = Math.min(BLOCK_LEN2, len - pos);
      for (let j = 0, posj; j < take; j++) {
        posj = pos + j;
        output[posj] = data[posj] ^ block[j];
      }
      pos += take;
    }
    return;
  }
  for (let pos = 0; pos < len; counter++) {
    core(sigma, key, nonce, b32, counter, rounds);
    if (counter >= MAX_COUNTER2)
      throw new Error("arx: counter overflow");
    const take = Math.min(BLOCK_LEN2, len - pos);
    if (isAligned && take === BLOCK_LEN2) {
      const pos32 = pos / 4;
      if (pos % 4 !== 0)
        throw new Error("arx: invalid block position");
      for (let j = 0, posj; j < BLOCK_LEN322; j++) {
        posj = pos32 + j;
        o32[posj] = d32[posj] ^ b32[j];
      }
      pos += BLOCK_LEN2;
      continue;
    }
    for (let j = 0, posj; j < take; j++) {
      posj = pos + j;
      output[posj] = data[posj] ^ block[j];
    }
    pos += take;
  }
}
function createCipher2(core, opts) {
  const { allowShortKeys, extendNonceFn, counterLength, counterRight, rounds } = checkOpts2({ allowShortKeys: false, counterLength: 8, counterRight: false, rounds: 20 }, opts);
  if (typeof core !== "function")
    throw new Error("core must be a function");
  anumber2(counterLength);
  anumber2(rounds);
  abool2(counterRight);
  abool2(allowShortKeys);
  return (key, nonce, data, output, counter = 0) => {
    abytes2(key, void 0, "key");
    abytes2(nonce, void 0, "nonce");
    abytes2(data, void 0, "data");
    const len = data.length;
    const hasOutput = output !== void 0;
    output = getOutput2(len, output, false);
    if (hasOutput)
      complexOverlapBytes(data, output);
    anumber2(counter);
    if (counter < 0 || counter >= MAX_COUNTER2)
      throw new Error("arx: counter overflow");
    const toClean = [];
    let l = key.length;
    let k;
    let sigma;
    if (l === 32) {
      toClean.push(k = copyBytes2(key));
      sigma = sigma32_322;
    } else if (l === 16 && allowShortKeys) {
      k = new Uint8Array(32);
      k.set(key);
      k.set(key, 16);
      sigma = sigma16_322;
      toClean.push(k);
    } else {
      abytes2(key, 32, "arx key");
      throw new Error("invalid key size");
    }
    if (!isLE2 || !isAligned323(nonce))
      toClean.push(nonce = copyBytes2(nonce));
    let k32 = u322(k);
    if (extendNonceFn) {
      if (nonce.length !== 24)
        throw new Error("arx: extended nonce must be 24 bytes");
      const n16 = nonce.subarray(0, 16);
      if (isLE2)
        extendNonceFn(sigma, k32, u322(n16), k32);
      else {
        const sigmaRaw = swap32IfBE2(Uint32Array.from(sigma));
        extendNonceFn(sigmaRaw, k32, u322(n16), k32);
        clean2(sigmaRaw);
        swap32IfBE2(k32);
      }
      nonce = nonce.subarray(16);
    } else if (!isLE2)
      swap32IfBE2(k32);
    const nonceNcLen = 16 - counterLength;
    if (nonceNcLen !== nonce.length)
      throw new Error(`arx: nonce must be ${nonceNcLen} or 16 bytes`);
    if (nonceNcLen !== 12) {
      const nc = new Uint8Array(12);
      nc.set(nonce, counterRight ? 0 : 12 - nonce.length);
      nonce = nc;
      toClean.push(nonce);
    }
    const n32 = swap32IfBE2(u322(nonce));
    try {
      runCipher2(core, sigma, k32, n32, data, output, counter, rounds);
      return output;
    } finally {
      clean2(...toClean);
    }
  };
}

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/ciphers/_poly1305.js
function u8to162(a, i) {
  return a[i++] & 255 | (a[i++] & 255) << 8;
}
var Poly13052 = class {
  blockLen = 16;
  outputLen = 16;
  buffer = new Uint8Array(16);
  r = new Uint16Array(10);
  // Allocating 1 array with .subarray() here is slower than 3
  h = new Uint16Array(10);
  pad = new Uint16Array(8);
  pos = 0;
  finished = false;
  destroyed = false;
  // Can be speed-up using BigUint64Array, at the cost of complexity
  constructor(key) {
    key = copyBytes2(abytes2(key, 32, "key"));
    const t0 = u8to162(key, 0);
    const t1 = u8to162(key, 2);
    const t2 = u8to162(key, 4);
    const t3 = u8to162(key, 6);
    const t4 = u8to162(key, 8);
    const t5 = u8to162(key, 10);
    const t6 = u8to162(key, 12);
    const t7 = u8to162(key, 14);
    this.r[0] = t0 & 8191;
    this.r[1] = (t0 >>> 13 | t1 << 3) & 8191;
    this.r[2] = (t1 >>> 10 | t2 << 6) & 7939;
    this.r[3] = (t2 >>> 7 | t3 << 9) & 8191;
    this.r[4] = (t3 >>> 4 | t4 << 12) & 255;
    this.r[5] = t4 >>> 1 & 8190;
    this.r[6] = (t4 >>> 14 | t5 << 2) & 8191;
    this.r[7] = (t5 >>> 11 | t6 << 5) & 8065;
    this.r[8] = (t6 >>> 8 | t7 << 8) & 8191;
    this.r[9] = t7 >>> 5 & 127;
    for (let i = 0; i < 8; i++)
      this.pad[i] = u8to162(key, 16 + 2 * i);
  }
  process(data, offset, isLast = false) {
    const hibit = isLast ? 0 : 1 << 11;
    const { h, r } = this;
    const r0 = r[0];
    const r1 = r[1];
    const r2 = r[2];
    const r3 = r[3];
    const r4 = r[4];
    const r5 = r[5];
    const r6 = r[6];
    const r7 = r[7];
    const r8 = r[8];
    const r9 = r[9];
    const t0 = u8to162(data, offset + 0);
    const t1 = u8to162(data, offset + 2);
    const t2 = u8to162(data, offset + 4);
    const t3 = u8to162(data, offset + 6);
    const t4 = u8to162(data, offset + 8);
    const t5 = u8to162(data, offset + 10);
    const t6 = u8to162(data, offset + 12);
    const t7 = u8to162(data, offset + 14);
    let h0 = h[0] + (t0 & 8191);
    let h1 = h[1] + ((t0 >>> 13 | t1 << 3) & 8191);
    let h2 = h[2] + ((t1 >>> 10 | t2 << 6) & 8191);
    let h3 = h[3] + ((t2 >>> 7 | t3 << 9) & 8191);
    let h4 = h[4] + ((t3 >>> 4 | t4 << 12) & 8191);
    let h5 = h[5] + (t4 >>> 1 & 8191);
    let h6 = h[6] + ((t4 >>> 14 | t5 << 2) & 8191);
    let h7 = h[7] + ((t5 >>> 11 | t6 << 5) & 8191);
    let h8 = h[8] + ((t6 >>> 8 | t7 << 8) & 8191);
    let h9 = h[9] + (t7 >>> 5 | hibit);
    let c = 0;
    let d0 = c + h0 * r0 + h1 * (5 * r9) + h2 * (5 * r8) + h3 * (5 * r7) + h4 * (5 * r6);
    c = d0 >>> 13;
    d0 &= 8191;
    d0 += h5 * (5 * r5) + h6 * (5 * r4) + h7 * (5 * r3) + h8 * (5 * r2) + h9 * (5 * r1);
    c += d0 >>> 13;
    d0 &= 8191;
    let d1 = c + h0 * r1 + h1 * r0 + h2 * (5 * r9) + h3 * (5 * r8) + h4 * (5 * r7);
    c = d1 >>> 13;
    d1 &= 8191;
    d1 += h5 * (5 * r6) + h6 * (5 * r5) + h7 * (5 * r4) + h8 * (5 * r3) + h9 * (5 * r2);
    c += d1 >>> 13;
    d1 &= 8191;
    let d2 = c + h0 * r2 + h1 * r1 + h2 * r0 + h3 * (5 * r9) + h4 * (5 * r8);
    c = d2 >>> 13;
    d2 &= 8191;
    d2 += h5 * (5 * r7) + h6 * (5 * r6) + h7 * (5 * r5) + h8 * (5 * r4) + h9 * (5 * r3);
    c += d2 >>> 13;
    d2 &= 8191;
    let d3 = c + h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * (5 * r9);
    c = d3 >>> 13;
    d3 &= 8191;
    d3 += h5 * (5 * r8) + h6 * (5 * r7) + h7 * (5 * r6) + h8 * (5 * r5) + h9 * (5 * r4);
    c += d3 >>> 13;
    d3 &= 8191;
    let d4 = c + h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;
    c = d4 >>> 13;
    d4 &= 8191;
    d4 += h5 * (5 * r9) + h6 * (5 * r8) + h7 * (5 * r7) + h8 * (5 * r6) + h9 * (5 * r5);
    c += d4 >>> 13;
    d4 &= 8191;
    let d5 = c + h0 * r5 + h1 * r4 + h2 * r3 + h3 * r2 + h4 * r1;
    c = d5 >>> 13;
    d5 &= 8191;
    d5 += h5 * r0 + h6 * (5 * r9) + h7 * (5 * r8) + h8 * (5 * r7) + h9 * (5 * r6);
    c += d5 >>> 13;
    d5 &= 8191;
    let d6 = c + h0 * r6 + h1 * r5 + h2 * r4 + h3 * r3 + h4 * r2;
    c = d6 >>> 13;
    d6 &= 8191;
    d6 += h5 * r1 + h6 * r0 + h7 * (5 * r9) + h8 * (5 * r8) + h9 * (5 * r7);
    c += d6 >>> 13;
    d6 &= 8191;
    let d7 = c + h0 * r7 + h1 * r6 + h2 * r5 + h3 * r4 + h4 * r3;
    c = d7 >>> 13;
    d7 &= 8191;
    d7 += h5 * r2 + h6 * r1 + h7 * r0 + h8 * (5 * r9) + h9 * (5 * r8);
    c += d7 >>> 13;
    d7 &= 8191;
    let d8 = c + h0 * r8 + h1 * r7 + h2 * r6 + h3 * r5 + h4 * r4;
    c = d8 >>> 13;
    d8 &= 8191;
    d8 += h5 * r3 + h6 * r2 + h7 * r1 + h8 * r0 + h9 * (5 * r9);
    c += d8 >>> 13;
    d8 &= 8191;
    let d9 = c + h0 * r9 + h1 * r8 + h2 * r7 + h3 * r6 + h4 * r5;
    c = d9 >>> 13;
    d9 &= 8191;
    d9 += h5 * r4 + h6 * r3 + h7 * r2 + h8 * r1 + h9 * r0;
    c += d9 >>> 13;
    d9 &= 8191;
    c = (c << 2) + c | 0;
    c = c + d0 | 0;
    d0 = c & 8191;
    c = c >>> 13;
    d1 += c;
    h[0] = d0;
    h[1] = d1;
    h[2] = d2;
    h[3] = d3;
    h[4] = d4;
    h[5] = d5;
    h[6] = d6;
    h[7] = d7;
    h[8] = d8;
    h[9] = d9;
  }
  finalize() {
    const { h, pad } = this;
    const g = new Uint16Array(10);
    let c = h[1] >>> 13;
    h[1] &= 8191;
    for (let i = 2; i < 10; i++) {
      h[i] += c;
      c = h[i] >>> 13;
      h[i] &= 8191;
    }
    h[0] += c * 5;
    c = h[0] >>> 13;
    h[0] &= 8191;
    h[1] += c;
    c = h[1] >>> 13;
    h[1] &= 8191;
    h[2] += c;
    g[0] = h[0] + 5;
    c = g[0] >>> 13;
    g[0] &= 8191;
    for (let i = 1; i < 10; i++) {
      g[i] = h[i] + c;
      c = g[i] >>> 13;
      g[i] &= 8191;
    }
    g[9] -= 1 << 13;
    let mask = (c ^ 1) - 1;
    for (let i = 0; i < 10; i++)
      g[i] &= mask;
    mask = ~mask;
    for (let i = 0; i < 10; i++)
      h[i] = h[i] & mask | g[i];
    h[0] = (h[0] | h[1] << 13) & 65535;
    h[1] = (h[1] >>> 3 | h[2] << 10) & 65535;
    h[2] = (h[2] >>> 6 | h[3] << 7) & 65535;
    h[3] = (h[3] >>> 9 | h[4] << 4) & 65535;
    h[4] = (h[4] >>> 12 | h[5] << 1 | h[6] << 14) & 65535;
    h[5] = (h[6] >>> 2 | h[7] << 11) & 65535;
    h[6] = (h[7] >>> 5 | h[8] << 8) & 65535;
    h[7] = (h[8] >>> 8 | h[9] << 5) & 65535;
    let f = h[0] + pad[0];
    h[0] = f & 65535;
    for (let i = 1; i < 8; i++) {
      f = (h[i] + pad[i] | 0) + (f >>> 16) | 0;
      h[i] = f & 65535;
    }
    clean2(g);
  }
  update(data) {
    aexists2(this);
    abytes2(data);
    data = copyBytes2(data);
    const { buffer, blockLen } = this;
    const len = data.length;
    for (let pos = 0; pos < len; ) {
      const take = Math.min(blockLen - this.pos, len - pos);
      if (take === blockLen) {
        for (; blockLen <= len - pos; pos += blockLen)
          this.process(data, pos);
        continue;
      }
      buffer.set(data.subarray(pos, pos + take), this.pos);
      this.pos += take;
      pos += take;
      if (this.pos === blockLen) {
        this.process(buffer, 0, false);
        this.pos = 0;
      }
    }
    return this;
  }
  destroy() {
    this.destroyed = true;
    clean2(this.h, this.r, this.buffer, this.pad);
  }
  digestInto(out) {
    aexists2(this);
    aoutput2(out, this);
    this.finished = true;
    const { buffer, h } = this;
    let { pos } = this;
    if (pos) {
      buffer[pos++] = 1;
      for (; pos < 16; pos++)
        buffer[pos] = 0;
      this.process(buffer, 0, true);
    }
    this.finalize();
    let opos = 0;
    for (let i = 0; i < 8; i++) {
      out[opos++] = h[i] >>> 0;
      out[opos++] = h[i] >>> 8;
    }
  }
  digest() {
    const { buffer, outputLen } = this;
    this.digestInto(buffer);
    const res = buffer.slice(0, outputLen);
    this.destroy();
    return res;
  }
};
var poly13052 = /* @__PURE__ */ wrapMacConstructor(32, (key) => new Poly13052(key));

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/ciphers/chacha.js
function chachaCore2(s, k, n, out, cnt, rounds = 20) {
  let y00 = s[0], y01 = s[1], y02 = s[2], y03 = s[3], y04 = k[0], y05 = k[1], y06 = k[2], y07 = k[3], y08 = k[4], y09 = k[5], y10 = k[6], y11 = k[7], y12 = cnt, y13 = n[0], y14 = n[1], y15 = n[2];
  let x00 = y00, x01 = y01, x02 = y02, x03 = y03, x04 = y04, x05 = y05, x06 = y06, x07 = y07, x08 = y08, x09 = y09, x10 = y10, x11 = y11, x12 = y12, x13 = y13, x14 = y14, x15 = y15;
  for (let r = 0; r < rounds; r += 2) {
    x00 = x00 + x04 | 0;
    x12 = rotl2(x12 ^ x00, 16);
    x08 = x08 + x12 | 0;
    x04 = rotl2(x04 ^ x08, 12);
    x00 = x00 + x04 | 0;
    x12 = rotl2(x12 ^ x00, 8);
    x08 = x08 + x12 | 0;
    x04 = rotl2(x04 ^ x08, 7);
    x01 = x01 + x05 | 0;
    x13 = rotl2(x13 ^ x01, 16);
    x09 = x09 + x13 | 0;
    x05 = rotl2(x05 ^ x09, 12);
    x01 = x01 + x05 | 0;
    x13 = rotl2(x13 ^ x01, 8);
    x09 = x09 + x13 | 0;
    x05 = rotl2(x05 ^ x09, 7);
    x02 = x02 + x06 | 0;
    x14 = rotl2(x14 ^ x02, 16);
    x10 = x10 + x14 | 0;
    x06 = rotl2(x06 ^ x10, 12);
    x02 = x02 + x06 | 0;
    x14 = rotl2(x14 ^ x02, 8);
    x10 = x10 + x14 | 0;
    x06 = rotl2(x06 ^ x10, 7);
    x03 = x03 + x07 | 0;
    x15 = rotl2(x15 ^ x03, 16);
    x11 = x11 + x15 | 0;
    x07 = rotl2(x07 ^ x11, 12);
    x03 = x03 + x07 | 0;
    x15 = rotl2(x15 ^ x03, 8);
    x11 = x11 + x15 | 0;
    x07 = rotl2(x07 ^ x11, 7);
    x00 = x00 + x05 | 0;
    x15 = rotl2(x15 ^ x00, 16);
    x10 = x10 + x15 | 0;
    x05 = rotl2(x05 ^ x10, 12);
    x00 = x00 + x05 | 0;
    x15 = rotl2(x15 ^ x00, 8);
    x10 = x10 + x15 | 0;
    x05 = rotl2(x05 ^ x10, 7);
    x01 = x01 + x06 | 0;
    x12 = rotl2(x12 ^ x01, 16);
    x11 = x11 + x12 | 0;
    x06 = rotl2(x06 ^ x11, 12);
    x01 = x01 + x06 | 0;
    x12 = rotl2(x12 ^ x01, 8);
    x11 = x11 + x12 | 0;
    x06 = rotl2(x06 ^ x11, 7);
    x02 = x02 + x07 | 0;
    x13 = rotl2(x13 ^ x02, 16);
    x08 = x08 + x13 | 0;
    x07 = rotl2(x07 ^ x08, 12);
    x02 = x02 + x07 | 0;
    x13 = rotl2(x13 ^ x02, 8);
    x08 = x08 + x13 | 0;
    x07 = rotl2(x07 ^ x08, 7);
    x03 = x03 + x04 | 0;
    x14 = rotl2(x14 ^ x03, 16);
    x09 = x09 + x14 | 0;
    x04 = rotl2(x04 ^ x09, 12);
    x03 = x03 + x04 | 0;
    x14 = rotl2(x14 ^ x03, 8);
    x09 = x09 + x14 | 0;
    x04 = rotl2(x04 ^ x09, 7);
  }
  let oi = 0;
  out[oi++] = y00 + x00 | 0;
  out[oi++] = y01 + x01 | 0;
  out[oi++] = y02 + x02 | 0;
  out[oi++] = y03 + x03 | 0;
  out[oi++] = y04 + x04 | 0;
  out[oi++] = y05 + x05 | 0;
  out[oi++] = y06 + x06 | 0;
  out[oi++] = y07 + x07 | 0;
  out[oi++] = y08 + x08 | 0;
  out[oi++] = y09 + x09 | 0;
  out[oi++] = y10 + x10 | 0;
  out[oi++] = y11 + x11 | 0;
  out[oi++] = y12 + x12 | 0;
  out[oi++] = y13 + x13 | 0;
  out[oi++] = y14 + x14 | 0;
  out[oi++] = y15 + x15 | 0;
}
var chacha202 = /* @__PURE__ */ createCipher2(chachaCore2, {
  counterRight: false,
  counterLength: 4,
  allowShortKeys: false
});
var ZEROS162 = /* @__PURE__ */ new Uint8Array(16);
var updatePadded2 = (h, msg) => {
  h.update(msg);
  const leftover = msg.length % 16;
  if (leftover)
    h.update(ZEROS162.subarray(leftover));
};
var ZEROS322 = /* @__PURE__ */ new Uint8Array(32);
function computeTag2(fn, key, nonce, ciphertext, AAD) {
  if (AAD !== void 0)
    abytes2(AAD, void 0, "AAD");
  const authKey = fn(key, nonce, ZEROS322);
  const lengths = u64Lengths2(ciphertext.length, AAD ? AAD.length : 0, true);
  const h = poly13052.create(authKey);
  if (AAD)
    updatePadded2(h, AAD);
  updatePadded2(h, ciphertext);
  h.update(lengths);
  const res = h.digest();
  clean2(authKey, lengths);
  return res;
}
var _poly1305_aead2 = (xorStream) => (key, nonce, AAD) => {
  const tagLength = 16;
  return {
    encrypt(plaintext, output) {
      const plength = plaintext.length;
      output = getOutput2(plength + tagLength, output, false);
      output.set(plaintext);
      const oPlain = output.subarray(0, -tagLength);
      xorStream(key, nonce, oPlain, oPlain, 1);
      const tag = computeTag2(xorStream, key, nonce, oPlain, AAD);
      output.set(tag, plength);
      clean2(tag);
      return output;
    },
    decrypt(ciphertext, output) {
      output = getOutput2(ciphertext.length - tagLength, output, false);
      const data = ciphertext.subarray(0, -tagLength);
      const passedTag = ciphertext.subarray(-tagLength);
      const tag = computeTag2(xorStream, key, nonce, data, AAD);
      if (!equalBytes2(passedTag, tag)) {
        clean2(tag);
        throw new Error("invalid tag");
      }
      output.set(ciphertext.subarray(0, -tagLength));
      xorStream(key, nonce, output, output, 1);
      clean2(tag);
      return output;
    }
  };
};
var chacha20poly13052 = /* @__PURE__ */ wrapCipher2(
  { blockSize: 64, nonceLength: 12, tagLength: 16, withAAD: true },
  /* @__PURE__ */ _poly1305_aead2(chacha202)
);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/hashes/utils.js
function isBytes3(a) {
  return a instanceof Uint8Array || ArrayBuffer.isView(a) && a.constructor.name === "Uint8Array" && "BYTES_PER_ELEMENT" in a && a.BYTES_PER_ELEMENT === 1;
}
function anumber3(n, title = "") {
  if (typeof n !== "number") {
    const prefix = title && `"${title}" `;
    throw new TypeError(`${prefix}expected number, got ${typeof n}`);
  }
  if (!Number.isSafeInteger(n) || n < 0) {
    const prefix = title && `"${title}" `;
    throw new RangeError(`${prefix}expected integer >= 0, got ${n}`);
  }
}
function abytes3(value, length, title = "") {
  const bytes = isBytes3(value);
  const len = value?.length;
  const needsLen = length !== void 0;
  if (!bytes || needsLen && len !== length) {
    const prefix = title && `"${title}" `;
    const ofLen = needsLen ? ` of length ${length}` : "";
    const got = bytes ? `length=${len}` : `type=${typeof value}`;
    const message = prefix + "expected Uint8Array" + ofLen + ", got " + got;
    if (!bytes)
      throw new TypeError(message);
    throw new RangeError(message);
  }
  return value;
}
function ahash2(h) {
  if (typeof h !== "function" || typeof h.create !== "function")
    throw new TypeError("Hash must wrapped by utils.createHasher");
  anumber3(h.outputLen);
  anumber3(h.blockLen);
  if (h.outputLen < 1)
    throw new Error('"outputLen" must be >= 1');
  if (h.blockLen < 1)
    throw new Error('"blockLen" must be >= 1');
}
function aexists3(instance, checkFinished = true) {
  if (instance.destroyed)
    throw new Error("Hash instance has been destroyed");
  if (checkFinished && instance.finished)
    throw new Error("Hash#digest() has already been called");
}
function aoutput3(out, instance) {
  abytes3(out, void 0, "digestInto() output");
  const min = instance.outputLen;
  if (out.length < min) {
    throw new RangeError('"digestInto() output" expected to be of length >=' + min);
  }
}
function clean3(...arrays) {
  for (let i = 0; i < arrays.length; i++) {
    arrays[i].fill(0);
  }
}
function createView3(arr) {
  return new DataView(arr.buffer, arr.byteOffset, arr.byteLength);
}
function rotr2(word, shift) {
  return word << 32 - shift | word >>> shift;
}
function rotl3(word, shift) {
  return word << shift | word >>> 32 - shift >>> 0;
}
var hasHexBuiltin2 = /* @__PURE__ */ (() => (
  // @ts-ignore
  typeof Uint8Array.from([]).toHex === "function" && typeof Uint8Array.fromHex === "function"
))();
var hexes2 = /* @__PURE__ */ Array.from({ length: 256 }, (_, i) => i.toString(16).padStart(2, "0"));
function bytesToHex2(bytes) {
  abytes3(bytes);
  if (hasHexBuiltin2)
    return bytes.toHex();
  let hex = "";
  for (let i = 0; i < bytes.length; i++) {
    hex += hexes2[bytes[i]];
  }
  return hex;
}
var asciis2 = { _0: 48, _9: 57, A: 65, F: 70, a: 97, f: 102 };
function asciiToBase162(ch) {
  if (ch >= asciis2._0 && ch <= asciis2._9)
    return ch - asciis2._0;
  if (ch >= asciis2.A && ch <= asciis2.F)
    return ch - (asciis2.A - 10);
  if (ch >= asciis2.a && ch <= asciis2.f)
    return ch - (asciis2.a - 10);
  return;
}
function hexToBytes3(hex) {
  if (typeof hex !== "string")
    throw new TypeError("hex string expected, got " + typeof hex);
  if (hasHexBuiltin2) {
    try {
      return Uint8Array.fromHex(hex);
    } catch (error) {
      if (error instanceof SyntaxError)
        throw new RangeError(error.message);
      throw error;
    }
  }
  const hl = hex.length;
  const al = hl / 2;
  if (hl % 2)
    throw new RangeError("hex string expected, got unpadded hex of length " + hl);
  const array = new Uint8Array(al);
  for (let ai = 0, hi = 0; ai < al; ai++, hi += 2) {
    const n1 = asciiToBase162(hex.charCodeAt(hi));
    const n2 = asciiToBase162(hex.charCodeAt(hi + 1));
    if (n1 === void 0 || n2 === void 0) {
      const char = hex[hi] + hex[hi + 1];
      throw new RangeError('hex string expected, got non-hex character "' + char + '" at index ' + hi);
    }
    array[ai] = n1 * 16 + n2;
  }
  return array;
}
function utf8ToBytes(str) {
  if (typeof str !== "string")
    throw new TypeError("string expected");
  return new Uint8Array(new TextEncoder().encode(str));
}
function concatBytes(...arrays) {
  let sum = 0;
  for (let i = 0; i < arrays.length; i++) {
    const a = arrays[i];
    abytes3(a);
    sum += a.length;
  }
  const res = new Uint8Array(sum);
  for (let i = 0, pad = 0; i < arrays.length; i++) {
    const a = arrays[i];
    res.set(a, pad);
    pad += a.length;
  }
  return res;
}
function createHasher2(hashCons, info = {}) {
  const hashC = (msg, opts) => hashCons(opts).update(msg).digest();
  const tmp = hashCons(void 0);
  hashC.outputLen = tmp.outputLen;
  hashC.blockLen = tmp.blockLen;
  hashC.canXOF = tmp.canXOF;
  hashC.create = (opts) => hashCons(opts);
  Object.assign(hashC, info);
  return Object.freeze(hashC);
}
var oidNist2 = (suffix) => ({
  // Current NIST hashAlgs suffixes used here fit in one DER subidentifier octet.
  // Larger suffix values would need base-128 OID encoding and a different length byte.
  oid: Uint8Array.from([6, 9, 96, 134, 72, 1, 101, 3, 4, 2, suffix])
});

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/hashes/hmac.js
var _HMAC2 = class {
  oHash;
  iHash;
  blockLen;
  outputLen;
  canXOF = false;
  finished = false;
  destroyed = false;
  constructor(hash, key) {
    ahash2(hash);
    abytes3(key, void 0, "key");
    this.iHash = hash.create();
    if (typeof this.iHash.update !== "function")
      throw new Error("Expected instance of class which extends utils.Hash");
    this.blockLen = this.iHash.blockLen;
    this.outputLen = this.iHash.outputLen;
    const blockLen = this.blockLen;
    const pad = new Uint8Array(blockLen);
    pad.set(key.length > blockLen ? hash.create().update(key).digest() : key);
    for (let i = 0; i < pad.length; i++)
      pad[i] ^= 54;
    this.iHash.update(pad);
    this.oHash = hash.create();
    for (let i = 0; i < pad.length; i++)
      pad[i] ^= 54 ^ 92;
    this.oHash.update(pad);
    clean3(pad);
  }
  update(buf) {
    aexists3(this);
    this.iHash.update(buf);
    return this;
  }
  digestInto(out) {
    aexists3(this);
    aoutput3(out, this);
    this.finished = true;
    const buf = out.subarray(0, this.outputLen);
    this.iHash.digestInto(buf);
    this.oHash.update(buf);
    this.oHash.digestInto(buf);
    this.destroy();
  }
  digest() {
    const out = new Uint8Array(this.oHash.outputLen);
    this.digestInto(out);
    return out;
  }
  _cloneInto(to) {
    to ||= Object.create(Object.getPrototypeOf(this), {});
    const { oHash, iHash, finished, destroyed, blockLen, outputLen } = this;
    to = to;
    to.finished = finished;
    to.destroyed = destroyed;
    to.blockLen = blockLen;
    to.outputLen = outputLen;
    to.oHash = oHash._cloneInto(to.oHash);
    to.iHash = iHash._cloneInto(to.iHash);
    return to;
  }
  clone() {
    return this._cloneInto();
  }
  destroy() {
    this.destroyed = true;
    this.oHash.destroy();
    this.iHash.destroy();
  }
};
var hmac2 = /* @__PURE__ */ (() => {
  const hmac_ = ((hash, key, message) => new _HMAC2(hash, key).update(message).digest());
  hmac_.create = (hash, key) => new _HMAC2(hash, key);
  return hmac_;
})();

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/hashes/hkdf.js
function extract(hash, ikm, salt) {
  ahash2(hash);
  if (salt === void 0)
    salt = new Uint8Array(hash.outputLen);
  return hmac2(hash, salt, ikm);
}
var HKDF_COUNTER = /* @__PURE__ */ Uint8Array.of(0);
var EMPTY_BUFFER = /* @__PURE__ */ Uint8Array.of();
function expand(hash, prk, info, length = 32) {
  ahash2(hash);
  anumber3(length, "length");
  abytes3(prk, void 0, "prk");
  const olen = hash.outputLen;
  if (prk.length < olen)
    throw new Error('"prk" must be at least HashLen octets');
  if (length > 255 * olen)
    throw new Error("Length must be <= 255*HashLen");
  const blocks = Math.ceil(length / olen);
  if (info === void 0)
    info = EMPTY_BUFFER;
  else
    abytes3(info, void 0, "info");
  const okm = new Uint8Array(blocks * olen);
  const HMAC = hmac2.create(hash, prk);
  const HMACTmp = HMAC._cloneInto();
  const T = new Uint8Array(HMAC.outputLen);
  for (let counter = 0; counter < blocks; counter++) {
    HKDF_COUNTER[0] = counter + 1;
    HMACTmp.update(counter === 0 ? EMPTY_BUFFER : T).update(info).update(HKDF_COUNTER).digestInto(T);
    okm.set(T, olen * counter);
    HMAC._cloneInto(HMACTmp);
  }
  HMAC.destroy();
  HMACTmp.destroy();
  clean3(T, HKDF_COUNTER);
  return okm.slice(0, length);
}
var hkdf = (hash, ikm, salt, info, length) => expand(hash, extract(hash, ikm, salt), info, length);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/hashes/_md.js
function Chi2(a, b, c) {
  return a & b ^ ~a & c;
}
function Maj2(a, b, c) {
  return a & b ^ a & c ^ b & c;
}
var HashMD2 = class {
  blockLen;
  outputLen;
  canXOF = false;
  padOffset;
  isLE;
  // For partial updates less than block size
  buffer;
  view;
  finished = false;
  length = 0;
  pos = 0;
  destroyed = false;
  constructor(blockLen, outputLen, padOffset, isLE3) {
    this.blockLen = blockLen;
    this.outputLen = outputLen;
    this.padOffset = padOffset;
    this.isLE = isLE3;
    this.buffer = new Uint8Array(blockLen);
    this.view = createView3(this.buffer);
  }
  update(data) {
    aexists3(this);
    abytes3(data);
    const { view, buffer, blockLen } = this;
    const len = data.length;
    for (let pos = 0; pos < len; ) {
      const take = Math.min(blockLen - this.pos, len - pos);
      if (take === blockLen) {
        const dataView = createView3(data);
        for (; blockLen <= len - pos; pos += blockLen)
          this.process(dataView, pos);
        continue;
      }
      buffer.set(data.subarray(pos, pos + take), this.pos);
      this.pos += take;
      pos += take;
      if (this.pos === blockLen) {
        this.process(view, 0);
        this.pos = 0;
      }
    }
    this.length += data.length;
    this.roundClean();
    return this;
  }
  digestInto(out) {
    aexists3(this);
    aoutput3(out, this);
    this.finished = true;
    const { buffer, view, blockLen, isLE: isLE3 } = this;
    let { pos } = this;
    buffer[pos++] = 128;
    clean3(this.buffer.subarray(pos));
    if (this.padOffset > blockLen - pos) {
      this.process(view, 0);
      pos = 0;
    }
    for (let i = pos; i < blockLen; i++)
      buffer[i] = 0;
    view.setBigUint64(blockLen - 8, BigInt(this.length * 8), isLE3);
    this.process(view, 0);
    const oview = createView3(out);
    const len = this.outputLen;
    if (len % 4)
      throw new Error("_sha2: outputLen must be aligned to 32bit");
    const outLen = len / 4;
    const state = this.get();
    if (outLen > state.length)
      throw new Error("_sha2: outputLen bigger than state");
    for (let i = 0; i < outLen; i++)
      oview.setUint32(4 * i, state[i], isLE3);
  }
  digest() {
    const { buffer, outputLen } = this;
    this.digestInto(buffer);
    const res = buffer.slice(0, outputLen);
    this.destroy();
    return res;
  }
  _cloneInto(to) {
    to ||= new this.constructor();
    to.set(...this.get());
    const { blockLen, buffer, length, finished, destroyed, pos } = this;
    to.destroyed = destroyed;
    to.finished = finished;
    to.length = length;
    to.pos = pos;
    if (length % blockLen)
      to.buffer.set(buffer);
    return to;
  }
  clone() {
    return this._cloneInto();
  }
};
var SHA256_IV2 = /* @__PURE__ */ Uint32Array.from([
  1779033703,
  3144134277,
  1013904242,
  2773480762,
  1359893119,
  2600822924,
  528734635,
  1541459225
]);

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/hashes/sha2.js
var SHA256_K2 = /* @__PURE__ */ Uint32Array.from([
  1116352408,
  1899447441,
  3049323471,
  3921009573,
  961987163,
  1508970993,
  2453635748,
  2870763221,
  3624381080,
  310598401,
  607225278,
  1426881987,
  1925078388,
  2162078206,
  2614888103,
  3248222580,
  3835390401,
  4022224774,
  264347078,
  604807628,
  770255983,
  1249150122,
  1555081692,
  1996064986,
  2554220882,
  2821834349,
  2952996808,
  3210313671,
  3336571891,
  3584528711,
  113926993,
  338241895,
  666307205,
  773529912,
  1294757372,
  1396182291,
  1695183700,
  1986661051,
  2177026350,
  2456956037,
  2730485921,
  2820302411,
  3259730800,
  3345764771,
  3516065817,
  3600352804,
  4094571909,
  275423344,
  430227734,
  506948616,
  659060556,
  883997877,
  958139571,
  1322822218,
  1537002063,
  1747873779,
  1955562222,
  2024104815,
  2227730452,
  2361852424,
  2428436474,
  2756734187,
  3204031479,
  3329325298
]);
var SHA256_W2 = /* @__PURE__ */ new Uint32Array(64);
var SHA2_32B2 = class extends HashMD2 {
  constructor(outputLen) {
    super(64, outputLen, 8, false);
  }
  get() {
    const { A, B, C, D, E, F, G, H } = this;
    return [A, B, C, D, E, F, G, H];
  }
  // prettier-ignore
  set(A, B, C, D, E, F, G, H) {
    this.A = A | 0;
    this.B = B | 0;
    this.C = C | 0;
    this.D = D | 0;
    this.E = E | 0;
    this.F = F | 0;
    this.G = G | 0;
    this.H = H | 0;
  }
  process(view, offset) {
    for (let i = 0; i < 16; i++, offset += 4)
      SHA256_W2[i] = view.getUint32(offset, false);
    for (let i = 16; i < 64; i++) {
      const W15 = SHA256_W2[i - 15];
      const W2 = SHA256_W2[i - 2];
      const s0 = rotr2(W15, 7) ^ rotr2(W15, 18) ^ W15 >>> 3;
      const s1 = rotr2(W2, 17) ^ rotr2(W2, 19) ^ W2 >>> 10;
      SHA256_W2[i] = s1 + SHA256_W2[i - 7] + s0 + SHA256_W2[i - 16] | 0;
    }
    let { A, B, C, D, E, F, G, H } = this;
    for (let i = 0; i < 64; i++) {
      const sigma1 = rotr2(E, 6) ^ rotr2(E, 11) ^ rotr2(E, 25);
      const T1 = H + sigma1 + Chi2(E, F, G) + SHA256_K2[i] + SHA256_W2[i] | 0;
      const sigma0 = rotr2(A, 2) ^ rotr2(A, 13) ^ rotr2(A, 22);
      const T2 = sigma0 + Maj2(A, B, C) | 0;
      H = G;
      G = F;
      F = E;
      E = D + T1 | 0;
      D = C;
      C = B;
      B = A;
      A = T1 + T2 | 0;
    }
    A = A + this.A | 0;
    B = B + this.B | 0;
    C = C + this.C | 0;
    D = D + this.D | 0;
    E = E + this.E | 0;
    F = F + this.F | 0;
    G = G + this.G | 0;
    H = H + this.H | 0;
    this.set(A, B, C, D, E, F, G, H);
  }
  roundClean() {
    clean3(SHA256_W2);
  }
  destroy() {
    this.destroyed = true;
    this.set(0, 0, 0, 0, 0, 0, 0, 0);
    clean3(this.buffer);
  }
};
var _SHA2562 = class extends SHA2_32B2 {
  // We cannot use array here since array allows indexing by variable
  // which means optimizer/compiler cannot use registers.
  A = SHA256_IV2[0] | 0;
  B = SHA256_IV2[1] | 0;
  C = SHA256_IV2[2] | 0;
  D = SHA256_IV2[3] | 0;
  E = SHA256_IV2[4] | 0;
  F = SHA256_IV2[5] | 0;
  G = SHA256_IV2[6] | 0;
  H = SHA256_IV2[7] | 0;
  constructor() {
    super(32);
  }
};
var sha2562 = /* @__PURE__ */ createHasher2(
  () => new _SHA2562(),
  /* @__PURE__ */ oidNist2(1)
);

// pinned:packages/web/src/entities/messenger/chat-real-crypto-provider.lib.ts
var KEY_BYTES = 32;
var TAG_BYTES = 16;
var GRANT_MAGIC = new Uint8Array([87, 83, 67, 71, 1]);
var GRANT_HEADER_BYTES = GRANT_MAGIC.length + KEY_BYTES;
var MAX_MESSAGE_BYTES = (2 ** 32 - 1) * 64;
var encoder = new TextEncoder();
function success(value) {
  return { ok: true, value };
}
function promisedResult(operation) {
  return Promise.resolve(operation());
}
function join(...parts) {
  const result = new Uint8Array(parts.reduce((length, part) => length + part.length, 0));
  let offset = 0;
  for (const part of parts) {
    result.set(part, offset);
    offset += part.length;
  }
  return result;
}
function sameBytes(left, right) {
  if (left.length !== right.length) return false;
  let difference = 0;
  for (let index = 0; index < left.length; index += 1) {
    difference |= left[index] ^ right[index];
  }
  return difference === 0;
}
function validKey(bytes) {
  return ArrayBuffer.isView(bytes) && Object.prototype.toString.call(bytes) === "[object Uint8Array]" && bytes.length === KEY_BYTES;
}
function validContext(context) {
  return context.streamUuid.length > 0 && context.keyUuid.length > 0;
}
function messageInfo(context) {
  return encoder.encode(
    JSON.stringify(["workspace-chat-crypto-v1", "message", context.streamUuid, context.keyUuid])
  );
}
function grantInfo(context) {
  return encoder.encode(
    JSON.stringify([
      "workspace-chat-crypto-v1",
      "grant",
      context.streamUuid,
      context.keyUuid,
      context.requestMessageUuid,
      context.senderDeviceId,
      context.recipientDeviceId
    ])
  );
}
function validGrantContext(context) {
  return validContext(context) && context.requestMessageUuid.length > 0 && context.senderDeviceId.length > 0 && context.recipientDeviceId.length > 0;
}
function readHeader(bytes, magic, headerBytes) {
  if (bytes.length < headerBytes + TAG_BYTES || !sameBytes(bytes.subarray(0, magic.length), magic)) {
    return null;
  }
  return bytes.subarray(0, headerBytes);
}
function randomKey() {
  return globalThis.crypto.getRandomValues(new Uint8Array(KEY_BYTES));
}
function createRealChatCryptoProvider() {
  const suite = new CipherSuite({
    kem: new DhkemX25519HkdfSha2562(),
    kdf: new HkdfSha256(),
    aead: new Chacha20Poly1305()
  });
  return {
    async createRequestKeyPair() {
      try {
        const pair = await suite.kem.generateKeyPair();
        const [publicKey, privateKey] = await Promise.all([
          suite.kem.serializePublicKey(pair.publicKey),
          suite.kem.serializePrivateKey(pair.privateKey)
        ]);
        return success({
          publicKey: { bytes: new Uint8Array(publicKey) },
          privateKey: new Uint8Array(privateKey)
        });
      } catch {
        return { ok: false, reason: "unavailable" };
      }
    },
    createGroupKey() {
      return promisedResult(() => {
        try {
          return success({ bytes: randomKey() });
        } catch {
          return { ok: false, reason: "unavailable" };
        }
      });
    },
    encryptMessage({ groupKey, plaintext, context }) {
      return promisedResult(() => {
        if (!validKey(groupKey.bytes)) return { ok: false, reason: "invalid-key" };
        if (!validContext(context)) return { ok: false, reason: "context-mismatch" };
        if (plaintext.length > MAX_MESSAGE_BYTES)
          return { ok: false, reason: "invalid-ciphertext" };
        let salt;
        try {
          salt = randomKey();
        } catch {
          return { ok: false, reason: "unavailable" };
        }
        const info = messageInfo(context);
        const messageKey = hkdf(sha2562, groupKey.bytes, salt, info, KEY_BYTES);
        try {
          const ciphertext = chacha20poly13052(
            messageKey,
            new Uint8Array(12),
            join(info, salt)
          ).encrypt(plaintext);
          return success({ bytes: join(salt, ciphertext) });
        } catch {
          return { ok: false, reason: "invalid-ciphertext" };
        } finally {
          messageKey.fill(0);
        }
      });
    },
    decryptMessage({ groupKey, envelope, context }) {
      return promisedResult(() => {
        if (!validKey(groupKey.bytes)) return { ok: false, reason: "invalid-key" };
        if (!validContext(context)) return { ok: false, reason: "context-mismatch" };
        if (envelope.bytes.length < KEY_BYTES + TAG_BYTES) {
          return { ok: false, reason: "invalid-ciphertext" };
        }
        const salt = envelope.bytes.subarray(0, KEY_BYTES);
        const ciphertext = envelope.bytes.subarray(KEY_BYTES);
        if (ciphertext.length - TAG_BYTES > MAX_MESSAGE_BYTES) {
          return { ok: false, reason: "invalid-ciphertext" };
        }
        const info = messageInfo(context);
        const messageKey = hkdf(sha2562, groupKey.bytes, salt, info, KEY_BYTES);
        try {
          return success(
            chacha20poly13052(messageKey, new Uint8Array(12), join(info, salt)).decrypt(ciphertext)
          );
        } catch {
          return { ok: false, reason: "invalid-ciphertext" };
        } finally {
          messageKey.fill(0);
        }
      });
    },
    async encryptGroupKeyGrant({ groupKey, recipientPublicKey, context }) {
      if (!validKey(groupKey.bytes) || !validKey(recipientPublicKey.bytes)) {
        return { ok: false, reason: "invalid-key" };
      }
      if (!validGrantContext(context)) return { ok: false, reason: "context-mismatch" };
      const secret = groupKey.bytes.slice();
      const publicBytes = recipientPublicKey.bytes.slice();
      const info = grantInfo(context);
      try {
        const recipientPublic = await suite.kem.deserializePublicKey(publicBytes);
        const sealed = await suite.seal(
          { recipientPublicKey: recipientPublic, info },
          secret,
          join(info, GRANT_MAGIC, publicBytes)
        );
        return success({
          bytes: join(GRANT_MAGIC, new Uint8Array(sealed.enc), new Uint8Array(sealed.ct))
        });
      } catch {
        return { ok: false, reason: "invalid-key" };
      } finally {
        secret.fill(0);
      }
    },
    async decryptGroupKeyGrant({ requestKeyPair, envelope, context }) {
      if (!validKey(requestKeyPair.privateKey) || !validKey(requestKeyPair.publicKey.bytes) || !validGrantContext(context))
        return { ok: false, reason: "invalid-key" };
      if (!validGrantContext(context)) return { ok: false, reason: "context-mismatch" };
      const bytes = envelope.bytes.slice();
      const header = readHeader(bytes, GRANT_MAGIC, GRANT_HEADER_BYTES);
      if (!header || bytes.length !== GRANT_HEADER_BYTES + KEY_BYTES + TAG_BYTES) {
        return { ok: false, reason: "invalid-ciphertext" };
      }
      const privateBytes = requestKeyPair.privateKey.slice();
      const publicBytes = requestKeyPair.publicKey.bytes.slice();
      const info = grantInfo(context);
      try {
        const recipientPrivate = await suite.kem.deserializePrivateKey(privateBytes);
        const opened = new Uint8Array(
          await suite.open(
            { recipientKey: recipientPrivate, enc: header.subarray(GRANT_MAGIC.length), info },
            bytes.subarray(GRANT_HEADER_BYTES),
            join(info, GRANT_MAGIC, publicBytes)
          )
        );
        if (!validKey(opened)) {
          opened.fill(0);
          return { ok: false, reason: "context-mismatch" };
        }
        return success({ bytes: opened });
      } catch {
        return { ok: false, reason: "invalid-ciphertext" };
      } finally {
        privateBytes.fill(0);
      }
    }
  };
}

// pinned:packages/web/src/entities/workspace-runtime/workspace-runtime.lib.ts
function workspaceRuntimeOwnerKey(owner) {
  return [
    "account",
    owner.accountId,
    "instance",
    owner.instanceId,
    "organization",
    owner.organizationId,
    "project",
    owner.projectId,
    "user",
    owner.userUuid
  ].join(":");
}

// platform:device-name
var getDeviceName = async () => "Synthetic JS issuer";

// pinned:packages/web/src/entities/messenger/messenger-adapters.lib.ts
function adaptMessengerMessagePayload(dto) {
  switch (dto.kind) {
    case "markdown":
      return { kind: "markdown", content: dto.content };
    case "e2ee":
      return { kind: dto.kind, keyUuid: dto.key_uuid, content: dto.content };
    case "key_request":
      return {
        kind: dto.kind,
        keyUuid: dto.key_uuid,
        deviceId: dto.device_id,
        deviceName: dto.device_name,
        publicKey: dto.public_key
      };
    case "key_announced":
      return { kind: dto.kind, keyUuid: dto.key_uuid };
    case "key_grant":
      return {
        kind: dto.kind,
        keyUuid: dto.key_uuid,
        requestMessageUuid: dto.request_message_uuid,
        senderDeviceId: dto.sender_device_id,
        recipientDeviceId: dto.recipient_device_id,
        content: dto.content
      };
    case "key_reject":
      return {
        kind: dto.kind,
        keyUuid: dto.key_uuid,
        requestMessageUuid: dto.request_message_uuid,
        senderDeviceId: dto.sender_device_id,
        recipientDeviceId: dto.recipient_device_id
      };
  }
}

// pinned:packages/web/src/entities/messenger/chat-e2ee-payload-adapters.lib.ts
function encodeBytes(bytes) {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}
function decodeBytes(value) {
  if (!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value) || value.length === 0)
    return null;
  try {
    const binary = atob(value);
    if (btoa(binary) !== value) return null;
    return Uint8Array.from(binary, (character) => character.charCodeAt(0));
  } catch {
    return null;
  }
}
function encodeChatE2eePayload(payload) {
  switch (payload.kind) {
    case "e2ee":
      return {
        kind: payload.kind,
        key_uuid: payload.keyUuid,
        content: encodeBytes(payload.content.bytes)
      };
    case "key_request":
      return {
        kind: payload.kind,
        key_uuid: payload.keyUuid,
        device_id: payload.deviceId,
        device_name: payload.deviceName,
        public_key: encodeBytes(payload.publicKey.bytes)
      };
    case "key_announced":
      return { kind: payload.kind, key_uuid: payload.keyUuid };
    case "key_grant":
      return {
        kind: payload.kind,
        key_uuid: payload.keyUuid,
        request_message_uuid: payload.requestMessageUuid,
        sender_device_id: payload.senderDeviceId,
        recipient_device_id: payload.recipientDeviceId,
        content: encodeBytes(payload.content.bytes)
      };
    case "key_reject":
      return {
        kind: payload.kind,
        key_uuid: payload.keyUuid,
        request_message_uuid: payload.requestMessageUuid,
        sender_device_id: payload.senderDeviceId,
        recipient_device_id: payload.recipientDeviceId
      };
  }
}
function decodeChatE2eePayload(dto) {
  const payload = adaptMessengerMessagePayload(dto);
  if (payload.kind === "markdown") throw new TypeError("Expected encrypted payload");
  return decodeMessengerE2eePayload(payload);
}
function decodeMessengerE2eePayload(payload) {
  if (payload.kind === "key_reject" || payload.kind === "key_announced")
    return { ok: true, value: payload };
  const field = payload.kind === "key_request" ? "public_key" : "content";
  const bytes = decodeBytes(payload.kind === "key_request" ? payload.publicKey : payload.content);
  if (bytes === null) return { ok: false, reason: "invalid-base64", field };
  return payload.kind === "key_request" ? { ok: true, value: { ...payload, publicKey: { bytes } } } : { ok: true, value: { ...payload, content: { bytes } } };
}

// pinned:packages/web/src/entities/messenger/chat-e2ee.lib.ts
function correlateChatKeyResponse(input) {
  const { request, response, issuerCanGrantKey } = input;
  if (request === null) return { status: "unresolved", reason: "request-not-loaded" };
  if (request.streamUuid !== response.streamUuid) {
    return { status: "ignored", reason: "stream-mismatch" };
  }
  if (request.uuid !== response.payload.requestMessageUuid) {
    return { status: "ignored", reason: "request-mismatch" };
  }
  if (request.payload.keyUuid !== response.payload.keyUuid) {
    return { status: "ignored", reason: "generation-mismatch" };
  }
  if (request.payload.deviceId !== response.payload.recipientDeviceId) {
    return { status: "ignored", reason: "recipient-mismatch" };
  }
  if (!issuerCanGrantKey) return { status: "ignored", reason: "issuer-not-allowed" };
  return {
    status: "matched",
    outcome: response.payload.kind === "key_grant" ? "grant-received" : "rejected"
  };
}
function resolveChatGenerationAccess(key) {
  if (key === null) return "missing";
  return key.groupKey === null ? "metadata-only" : "ready";
}

// pinned:packages/web/src/entities/messenger/chat-e2ee-controller.lib.ts
function equalBytes3(left, right) {
  return left.length === right.length && left.every((byte, index) => byte === right[index]);
}
var failure = (reason) => ({
  ok: false,
  reason
});
var success2 = (value) => ({ ok: true, value });
var ChatE2eeController = class {
  deps;
  disposed = false;
  pending = 0;
  key = null;
  keyRevision = 0;
  requestStatus = "idle";
  error = null;
  requests = /* @__PURE__ */ new Map();
  waiting = /* @__PURE__ */ new Map();
  processed = /* @__PURE__ */ new Set();
  queue = Promise.resolve();
  requestQueue = Promise.resolve();
  constructor(dependencies) {
    this.deps = {
      ...dependencies,
      scope: { ...dependencies.scope, owner: { ...dependencies.scope.owner } }
    };
  }
  isCurrent = () => {
    const current = this.deps.getCurrentScope();
    const captured = this.deps.scope;
    return !this.disposed && current !== null && workspaceRuntimeOwnerKey(current.owner) === workspaceRuntimeOwnerKey(captured.owner) && current.runtimeGeneration === captured.runtimeGeneration && current.deviceId === captured.deviceId && current.streamUuid === captured.streamUuid && current.keyUuid === captured.keyUuid && this.deps.device.deviceId === captured.deviceId;
  };
  invalidate() {
    this.disposed = true;
    this.key = null;
    this.requests.clear();
    this.waiting.clear();
    this.processed.clear();
  }
  getState() {
    if (!this.isCurrent())
      return { access: "missing", pending: false, requestStatus: "idle", error: "stale" };
    return {
      access: resolveChatGenerationAccess(this.key),
      pending: this.pending > 0,
      requestStatus: this.requestStatus,
      error: this.error
    };
  }
  async run(operation) {
    if (!this.isCurrent()) return failure("stale");
    this.pending++;
    try {
      const result = await operation();
      if (!this.isCurrent()) return failure("stale");
      this.error = result.ok ? null : result.reason;
      return result;
    } catch {
      if (!this.isCurrent()) return failure("stale");
      this.error = "unavailable";
      return failure("unavailable");
    } finally {
      this.pending--;
    }
  }
  async readKey() {
    try {
      const revision = this.keyRevision;
      const key = await this.deps.storage.readChatKey(this.deps.scope);
      if (!this.isCurrent()) return failure("stale");
      if (revision !== this.keyRevision) return success2(this.key);
      this.key = key;
      return success2(key);
    } catch {
      return failure("storage-failed");
    }
  }
  async readDeletionRevision() {
    try {
      const revision = await this.deps.storage.readChatKeyDeletionRevision(this.deps.scope);
      return this.isCurrent() ? success2(revision) : failure("stale");
    } catch {
      return failure("storage-failed");
    }
  }
  async checkDeletionRevision(revision) {
    const latest = await this.readDeletionRevision();
    if (!latest.ok) return latest;
    if (latest.value !== revision) {
      this.key = null;
      this.keyRevision++;
      return failure("missing-key");
    }
    return success2(void 0);
  }
  async send(payload, allowed, canonicalMessageUuid) {
    const guard = () => this.isCurrent() && allowed();
    if (!guard()) return failure(this.isCurrent() ? "forbidden" : "stale");
    try {
      const message = await this.deps.sendPayload(payload, guard, canonicalMessageUuid);
      if (!guard()) return failure(this.isCurrent() ? "forbidden" : "stale");
      if (message.streamUuid !== this.deps.scope.streamUuid || message.authorUuid !== this.deps.scope.owner.userUuid || message.payload.kind !== payload.kind || message.payload.keyUuid !== payload.keyUuid || JSON.stringify(encodeChatE2eePayload(message.payload)) !== JSON.stringify(encodeChatE2eePayload(payload)))
        return failure("invalid-message");
      return success2(message);
    } catch {
      return failure("send-failed");
    }
  }
  loadKey() {
    return this.run(async () => {
      const result = await this.readKey();
      if (!result.ok) return result;
      try {
        const pending = await this.deps.storage.listPendingRequests(this.deps.scope);
        if (!this.isCurrent()) return failure("stale");
        if (pending.length > 0 && this.requestStatus === "idle" && resolveChatGenerationAccess(this.key) !== "ready")
          this.requestStatus = "requested";
      } catch {
        return failure("storage-failed");
      }
      return success2(void 0);
    });
  }
  requestKey(recoveryEnvelope, allowUsableKey = false) {
    const next = this.requestQueue.then(
      () => this.run(async () => {
        const allowed = () => this.deps.getRights().canRequest;
        if (!allowed()) return failure("forbidden");
        let deletionRevision;
        try {
          deletionRevision = await this.deps.storage.readChatKeyDeletionRevision(this.deps.scope);
          if (!this.isCurrent()) return failure("stale");
        } catch {
          return failure("storage-failed");
        }
        let recovery;
        if (recoveryEnvelope) {
          const stored = await this.readKey();
          if (!stored.ok) return stored;
          if (stored.value?.groupKey) {
            const checked = await this.deps.provider.decryptMessage({
              groupKey: stored.value.groupKey,
              envelope: recoveryEnvelope,
              context: this.deps.scope
            });
            if (!this.isCurrent()) return failure("stale");
            if (checked.ok) {
              checked.value.fill(0);
              if (!allowUsableKey) return failure("invalid-message");
            } else if (checked.reason !== "invalid-ciphertext") return checked;
            if (!checked.ok)
              recovery = {
                envelope: { bytes: recoveryEnvelope.bytes.slice() },
                previousGroupKey: { bytes: stored.value.groupKey.bytes.slice() }
              };
          }
        }
        let pending;
        try {
          const existing = await this.deps.storage.listPendingRequests(this.deps.scope);
          if (!this.isCurrent()) return failure("stale");
          const oldest = existing.filter(
            (item) => !recovery || item.recovery !== void 0 && equalBytes3(item.recovery.envelope.bytes, recovery.envelope.bytes) && equalBytes3(
              item.recovery.previousGroupKey.bytes,
              recovery.previousGroupKey.bytes
            )
          ).sort(
            (left, right) => left.createdAt - right.createdAt || left.localRequestId.localeCompare(right.localRequestId)
          )[0];
          if (oldest) pending = oldest;
          else {
            const deviceName = await getDeviceName();
            if (!this.isCurrent()) return failure("stale");
            if (!allowed()) return failure("forbidden");
            const pair = await this.deps.provider.createRequestKeyPair();
            if (!this.isCurrent()) return failure("stale");
            if (!pair.ok) return pair;
            if (!allowed()) return failure("forbidden");
            const uuid2 = crypto.randomUUID();
            pending = {
              localRequestId: uuid2,
              requestMessageUuid: null,
              deviceName,
              publicKey: pair.value.publicKey,
              privateKey: pair.value.privateKey,
              createdAt: Date.now(),
              ...recovery ? { recovery } : {}
            };
            const committed = await this.deps.storage.writePendingRequest(
              this.deps.scope,
              pending,
              {
                isCurrent: () => this.isCurrent() && allowed(),
                expectedDeletionRevision: deletionRevision
              }
            );
            if (!this.isCurrent()) return failure("stale");
            if (!allowed()) return failure("forbidden");
            if (!committed) return failure("storage-failed");
          }
        } catch {
          return failure("storage-failed");
        }
        const uuid = pending.localRequestId;
        const result = await this.send(
          {
            kind: "key_request",
            keyUuid: this.deps.scope.keyUuid,
            deviceId: this.deps.device.deviceId,
            deviceName: pending.deviceName ?? this.deps.device.deviceName,
            publicKey: pending.publicKey
          },
          allowed,
          uuid
        );
        if (!result.ok || result.value.payload.kind !== "key_request") return result;
        if (!equalBytes3(result.value.payload.publicKey.bytes, pending.publicKey.bytes) || result.value.payload.deviceId !== this.deps.device.deviceId)
          return failure("invalid-message");
        try {
          const bound = await this.deps.storage.bindPendingRequest(
            this.deps.scope,
            pending.localRequestId,
            result.value.uuid,
            { isCurrent: () => this.isCurrent() && allowed() }
          );
          if (!this.isCurrent()) return failure("stale");
          if (!allowed()) return failure("forbidden");
          if (!bound) return failure("storage-failed");
        } catch {
          return failure("storage-failed");
        }
        const ingested = await this.ingestKeyMessage(result.value);
        if (!ingested.ok) return ingested;
        if (this.requestStatus === "idle") this.requestStatus = "requested";
        return result;
      })
    );
    this.requestQueue = next;
    return next;
  }
  validRequest(request) {
    return request.streamUuid === this.deps.scope.streamUuid && request.payload.keyUuid === this.deps.scope.keyUuid;
  }
  canRespond(action, request) {
    const rights = this.deps.getRights();
    return (action === "grant" ? rights.canGrant : rights.canReject) && (this.deps.isRequesterAllowed?.(request.authorUuid) ?? true);
  }
  grantKey(request) {
    return this.run(async () => {
      if (!this.validRequest(request)) return failure("invalid-message");
      if (!this.canRespond("grant", request)) return failure("forbidden");
      const deletionRevision = await this.readDeletionRevision();
      if (!deletionRevision.ok) return deletionRevision;
      const stored = await this.readKey();
      if (!stored.ok) return stored;
      if (stored.value?.groupKey == null) return failure("missing-key");
      if (!this.canRespond("grant", request)) return failure("forbidden");
      const context = {
        streamUuid: this.deps.scope.streamUuid,
        keyUuid: this.deps.scope.keyUuid,
        requestMessageUuid: request.uuid,
        senderDeviceId: this.deps.device.deviceId,
        recipientDeviceId: request.payload.deviceId
      };
      const envelope = await this.deps.provider.encryptGroupKeyGrant({
        groupKey: stored.value.groupKey,
        recipientPublicKey: request.payload.publicKey,
        context
      });
      if (!this.isCurrent()) return failure("stale");
      if (!envelope.ok) return envelope;
      const retained = await this.checkDeletionRevision(deletionRevision.value);
      if (!retained.ok) return retained;
      return this.send(
        {
          kind: "key_grant",
          keyUuid: context.keyUuid,
          requestMessageUuid: context.requestMessageUuid,
          senderDeviceId: context.senderDeviceId,
          recipientDeviceId: context.recipientDeviceId,
          content: envelope.value
        },
        () => this.canRespond("grant", request)
      );
    });
  }
  rejectKey(request) {
    return this.run(async () => {
      if (!this.validRequest(request)) return failure("invalid-message");
      return this.send(
        {
          kind: "key_reject",
          keyUuid: this.deps.scope.keyUuid,
          requestMessageUuid: request.uuid,
          senderDeviceId: this.deps.device.deviceId,
          recipientDeviceId: request.payload.deviceId
        },
        () => this.canRespond("reject", request)
      );
    });
  }
  encryptMessage(plaintext, dispatch, preparedPayload, verifyPreparedPlaintext = false) {
    return this.run(async () => {
      if (!this.deps.getRights().canSend) return failure("forbidden");
      const deletionRevision = await this.readDeletionRevision();
      if (!deletionRevision.ok) return deletionRevision;
      const stored = await this.readKey();
      if (!stored.ok) return stored;
      if (stored.value?.groupKey == null) return failure("missing-key");
      if (preparedPayload != null && preparedPayload.keyUuid !== this.deps.scope.keyUuid)
        return failure("invalid-key");
      if (preparedPayload && verifyPreparedPlaintext) {
        const verified = await this.deps.provider.decryptMessage({
          groupKey: stored.value.groupKey,
          envelope: preparedPayload.content,
          context: this.deps.scope
        });
        if (!this.isCurrent()) {
          if (verified.ok) verified.value.fill(0);
          return failure("stale");
        }
        if (!verified.ok) return failure("invalid-key");
        try {
          if (verified.value.length !== plaintext.length || verified.value.some((value, index) => value !== plaintext[index]))
            return failure("invalid-key");
        } finally {
          verified.value.fill(0);
        }
      }
      const encrypted = preparedPayload == null ? await this.deps.provider.encryptMessage({
        groupKey: stored.value.groupKey,
        plaintext,
        context: this.deps.scope
      }) : success2(preparedPayload.content);
      if (!this.isCurrent()) return failure("stale");
      if (!encrypted.ok) return encrypted;
      const retained = await this.checkDeletionRevision(deletionRevision.value);
      if (!retained.ok) return retained;
      const guard = () => this.isCurrent() && this.deps.getRights().canSend;
      if (!guard()) return failure(this.isCurrent() ? "forbidden" : "stale");
      try {
        const sent = await dispatch(
          { kind: "e2ee", keyUuid: this.deps.scope.keyUuid, content: encrypted.value },
          guard
        );
        if (!guard()) return failure(this.isCurrent() ? "forbidden" : "stale");
        return success2(sent);
      } catch {
        return failure("send-failed");
      }
    });
  }
  ingestKeyMessage(message) {
    const next = this.queue.then(() => this.run(() => this.ingest(message)));
    this.queue = next;
    return next;
  }
  async ingest(message) {
    if (message.streamUuid !== this.deps.scope.streamUuid || message.payload.keyUuid !== this.deps.scope.keyUuid)
      return failure("invalid-message");
    if (message.payload.kind === "key_request")
      return this.receiveRequest({ ...message, payload: message.payload });
    if (message.payload.kind === "key_grant" || message.payload.kind === "key_reject")
      return this.receiveResponse({ ...message, payload: message.payload });
    if (message.payload.kind !== "key_announced") return failure("invalid-message");
    const allowed = () => this.deps.isAnnouncerAllowed(message.authorUuid);
    if (!allowed()) return failure("forbidden");
    return this.drainWaiting();
  }
  async receiveRequest(request) {
    if (!this.validRequest(request)) return failure("invalid-message");
    if (request.authorUuid === this.deps.scope.owner.userUuid && request.payload.deviceId === this.deps.scope.deviceId) {
      try {
        const pending = await this.deps.storage.listPendingRequests(this.deps.scope);
        if (!this.isCurrent()) return failure("stale");
        const matchingKeys = pending.filter(
          (item) => equalBytes3(item.publicKey.bytes, request.payload.publicKey.bytes)
        );
        const bound = matchingKeys.filter((item) => item.requestMessageUuid === request.uuid);
        const candidates = bound.length ? bound : matchingKeys.filter(
          (item) => item.requestMessageUuid === null || item.requestMessageUuid === item.localRequestId
        );
        if (candidates.length > 1) return failure("invalid-message");
        const candidate = candidates[0];
        if (candidate && candidate.requestMessageUuid !== request.uuid) {
          const bound2 = await this.deps.storage.bindPendingRequest(
            this.deps.scope,
            candidate.localRequestId,
            request.uuid,
            { isCurrent: this.isCurrent }
          );
          if (!this.isCurrent()) return failure("stale");
          if (!bound2) return failure("storage-failed");
        }
      } catch {
        return failure("storage-failed");
      }
    }
    this.requests.set(request.uuid, request);
    return this.drainWaiting();
  }
  async drainWaiting() {
    let firstFailure = null;
    for (const response of this.waiting.values()) {
      const result = await this.receiveResponse(response);
      if (!result.ok) firstFailure ??= result;
      if (!this.isCurrent()) return failure("stale");
    }
    return firstFailure ?? success2(void 0);
  }
  async receiveResponse(response) {
    if (this.processed.has(response.uuid)) return success2(void 0);
    const allowed = () => this.deps.isIssuerAllowed(response.authorUuid);
    if (response.payload.recipientDeviceId !== this.deps.scope.deviceId) {
      this.waiting.delete(response.uuid);
      return failure("invalid-message");
    }
    let pending;
    try {
      const candidates = await this.deps.storage.listPendingRequests(this.deps.scope);
      if (!this.isCurrent()) return failure("stale");
      pending = candidates.find(
        (item) => item.requestMessageUuid === response.payload.requestMessageUuid
      );
      if (!pending) {
        const completion2 = await this.deps.storage.readRequestCompletionByMessageUuid(
          this.deps.scope,
          response.payload.requestMessageUuid
        );
        if (!this.isCurrent()) return failure("stale");
        if (completion2?.requestMessageUuid === response.payload.requestMessageUuid) {
          this.processed.add(response.uuid);
          this.waiting.delete(response.uuid);
          return success2(void 0);
        }
      }
    } catch {
      return failure("storage-failed");
    }
    const request = this.requests.get(response.payload.requestMessageUuid) ?? null;
    if (!pending) {
      this.waiting.set(response.uuid, response);
      return success2(void 0);
    }
    if (!allowed()) return failure("forbidden");
    if (request) {
      const correlation = correlateChatKeyResponse({
        request,
        response,
        issuerCanGrantKey: allowed()
      });
      if (correlation.status !== "matched" || request.authorUuid !== this.deps.scope.owner.userUuid || !equalBytes3(request.payload.publicKey.bytes, pending.publicKey.bytes)) {
        this.waiting.delete(response.uuid);
        return failure("invalid-message");
      }
    }
    const completion = {
      localRequestId: pending.localRequestId,
      requestMessageUuid: response.payload.requestMessageUuid,
      responseMessageUuid: response.uuid
    };
    const guard = () => this.isCurrent() && allowed();
    if (response.payload.kind === "key_reject") {
      try {
        const committed = await this.deps.storage.completeReject(this.deps.scope, completion, {
          isCurrent: guard
        });
        if (!this.isCurrent()) return failure("stale");
        if (!allowed()) return failure("forbidden");
        if (!committed) {
          const receipt = await this.deps.storage.readRequestCompletion(
            this.deps.scope,
            pending.localRequestId
          );
          if (!this.isCurrent()) return failure("stale");
          if (!allowed()) return failure("forbidden");
          if (receipt?.outcome !== "rejected" || receipt.requestMessageUuid !== completion.requestMessageUuid)
            return failure("invalid-message");
        }
      } catch {
        return failure("storage-failed");
      }
      if (resolveChatGenerationAccess(this.key) !== "ready") this.requestStatus = "rejected";
      this.processed.add(response.uuid);
      this.waiting.delete(response.uuid);
      return success2(void 0);
    }
    const payload = response.payload;
    const decrypted = await this.deps.provider.decryptGroupKeyGrant({
      requestKeyPair: { publicKey: pending.publicKey, privateKey: pending.privateKey },
      envelope: payload.content,
      context: {
        streamUuid: response.streamUuid,
        keyUuid: payload.keyUuid,
        requestMessageUuid: payload.requestMessageUuid,
        senderDeviceId: payload.senderDeviceId,
        recipientDeviceId: payload.recipientDeviceId
      }
    });
    if (!this.isCurrent()) return failure("stale");
    if (!decrypted.ok) return decrypted;
    if (!allowed()) return failure("forbidden");
    try {
      if (pending.recovery && !equalBytes3(decrypted.value.bytes, pending.recovery.previousGroupKey.bytes)) {
        const checked = await this.deps.provider.decryptMessage({
          groupKey: decrypted.value,
          envelope: pending.recovery.envelope,
          context: this.deps.scope
        });
        if (!this.isCurrent()) return failure("stale");
        if (!allowed()) return failure("forbidden");
        if (!checked.ok) {
          const committed2 = await this.deps.storage.completeReject(this.deps.scope, completion, {
            isCurrent: guard
          });
          if (!this.isCurrent()) return failure("stale");
          if (!allowed()) return failure("forbidden");
          if (!committed2) return failure("storage-failed");
          this.processed.add(response.uuid);
          this.waiting.delete(response.uuid);
          this.requestStatus = "rejected";
          return checked;
        }
        checked.value.fill(0);
      }
      const committed = await this.deps.storage.completeGrant(
        this.deps.scope,
        {
          ...completion,
          groupKey: decrypted.value,
          ...pending.recovery ? { replaceExistingKey: true } : {}
        },
        { isCurrent: guard }
      );
      if (!this.isCurrent()) return failure("stale");
      if (!allowed()) return failure("forbidden");
      if (!committed) {
        const receipt = await this.deps.storage.readRequestCompletion(
          this.deps.scope,
          pending.localRequestId
        );
        if (!this.isCurrent()) return failure("stale");
        if (!allowed()) return failure("forbidden");
        if (receipt?.outcome !== "granted" || receipt.requestMessageUuid !== completion.requestMessageUuid)
          return failure("invalid-message");
        const key = await this.readKey();
        if (!key.ok) return key;
        if (key.value?.groupKey == null) return failure("missing-key");
      }
    } catch {
      return failure("storage-failed");
    }
    this.keyRevision++;
    const stored = await this.readKey();
    if (!stored.ok) return stored;
    if (!allowed()) return failure("forbidden");
    if (stored.value?.groupKey == null) return failure("missing-key");
    this.requestStatus = "grant-received";
    this.processed.add(response.uuid);
    this.waiting.delete(response.uuid);
    return success2(void 0);
  }
};

// ../../../../../../../../worktrees/cassi-telegram-ui/node_modules/@noble/hashes/legacy.js
var SHA1_IV = /* @__PURE__ */ Uint32Array.from([
  1732584193,
  4023233417,
  2562383102,
  271733878,
  3285377520
]);
var SHA1_W = /* @__PURE__ */ new Uint32Array(80);
var _SHA1 = class extends HashMD2 {
  A = SHA1_IV[0] | 0;
  B = SHA1_IV[1] | 0;
  C = SHA1_IV[2] | 0;
  D = SHA1_IV[3] | 0;
  E = SHA1_IV[4] | 0;
  constructor() {
    super(64, 20, 8, false);
  }
  get() {
    const { A, B, C, D, E } = this;
    return [A, B, C, D, E];
  }
  set(A, B, C, D, E) {
    this.A = A | 0;
    this.B = B | 0;
    this.C = C | 0;
    this.D = D | 0;
    this.E = E | 0;
  }
  process(view, offset) {
    for (let i = 0; i < 16; i++, offset += 4)
      SHA1_W[i] = view.getUint32(offset, false);
    for (let i = 16; i < 80; i++)
      SHA1_W[i] = rotl3(SHA1_W[i - 3] ^ SHA1_W[i - 8] ^ SHA1_W[i - 14] ^ SHA1_W[i - 16], 1);
    let { A, B, C, D, E } = this;
    for (let i = 0; i < 80; i++) {
      let F, K;
      if (i < 20) {
        F = Chi2(B, C, D);
        K = 1518500249;
      } else if (i < 40) {
        F = B ^ C ^ D;
        K = 1859775393;
      } else if (i < 60) {
        F = Maj2(B, C, D);
        K = 2400959708;
      } else {
        F = B ^ C ^ D;
        K = 3395469782;
      }
      const T = rotl3(A, 5) + F + E + K + SHA1_W[i] | 0;
      E = D;
      D = C;
      C = rotl3(B, 30);
      B = A;
      A = T;
    }
    A = A + this.A | 0;
    B = B + this.B | 0;
    C = C + this.C | 0;
    D = D + this.D | 0;
    E = E + this.E | 0;
    this.set(A, B, C, D, E);
  }
  roundClean() {
    clean3(SHA1_W);
  }
  destroy() {
    this.destroyed = true;
    this.set(0, 0, 0, 0, 0);
    clean3(this.buffer);
  }
};
var sha1 = /* @__PURE__ */ createHasher2(() => new _SHA1());

// pinned:packages/web/src/entities/messenger/messenger-message-identity.lib.ts
var UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
function normalizeMessengerMessageUuid(uuid) {
  if (!UUID_PATTERN.test(uuid)) {
    throw new TypeError(`Expected a hyphenated UUID, received: ${uuid}`);
  }
  return uuid.toLowerCase();
}
function uuidBytes(uuid) {
  return hexToBytes3(uuid.replaceAll("-", ""));
}
function formatUuid(bytes) {
  const hex = bytesToHex2(bytes);
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20, 32)}`;
}
function createMessengerPlacementUuid(topicUuid, canonicalMessageUuid) {
  const normalizedTopicUuid = normalizeMessengerMessageUuid(topicUuid);
  const normalizedCanonicalMessageUuid = normalizeMessengerMessageUuid(canonicalMessageUuid);
  const digest = sha1(
    concatBytes(uuidBytes(normalizedTopicUuid), utf8ToBytes(normalizedCanonicalMessageUuid))
  ).slice(0, 16);
  digest[6] = (digest[6] ?? 0) & 15 | 80;
  digest[8] = (digest[8] ?? 0) & 63 | 128;
  return formatUuid(digest);
}

// peer.ts
async function verifyNativeSendMode() {
  const maximumLineBytes = 512 * 1024;
  async function* frames() {
    let buffered = Buffer.alloc(0);
    for await (const chunk of process.stdin) {
      const bytes = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
      if (buffered.length + bytes.length > maximumLineBytes + 65536) throw new Error("Synthetic peer frame too large");
      buffered = Buffer.concat([buffered, bytes]);
      for (let end; (end = buffered.indexOf(10)) !== -1; ) {
        if (end === 0 || end > maximumLineBytes) throw new Error("Synthetic peer frame invalid");
        const line = buffered.subarray(0, end);
        buffered = buffered.subarray(end + 1);
        yield JSON.parse(line.toString("utf8"));
      }
      if (buffered.length > maximumLineBytes) throw new Error("Synthetic peer frame too large");
    }
    if (buffered.length !== 0) throw new Error("Synthetic peer frame incomplete");
  }
  const iterator = frames();
  const first = await iterator.next();
  if (first.done) throw new Error("Synthetic peer input missing");
  const input = first.value;
  if (input.reject || typeof input.plaintext !== "string" || typeof input.targetTopic !== "string" || input.readPlaintext !== void 0 && typeof input.readPlaintext !== "string")
    throw new Error("Synthetic peer input invalid");
  const provider = createRealChatCryptoProvider();
  const key = await provider.createGroupKey();
  if (!key.ok) throw new Error("Synthetic peer key unavailable");
  const expected = new TextEncoder().encode(input.plaintext);
  const readPlaintext = new TextEncoder().encode(input.readPlaintext ?? input.plaintext);
  let controller;
  let cleartext;
  try {
    const owner = { accountId: "synthetic", instanceId: "synthetic", organizationId: "synthetic", projectId: input.project, userUuid: input.author };
    const scope = { owner, deviceId: input.senderDevice, streamUuid: input.stream, keyUuid: input.key, runtimeGeneration: 1 };
    const manager = input.role === "owner" || input.role === "administrator";
    controller = new ChatE2eeController({
      scope,
      device: { deviceId: scope.deviceId, deviceName: "Synthetic JS issuer" },
      provider,
      storage: { readChatKey: async () => ({ groupKey: key.value }), readChatKeyDeletionRevision: async () => 0 },
      getCurrentScope: () => scope,
      getRights: () => ({ canRequest: true, canSend: true, canGrant: manager, canReject: manager, canRotate: input.role === "owner" }),
      isIssuerAllowed: () => manager,
      isAnnouncerAllowed: () => manager,
      isRequesterAllowed: () => true,
      sendPayload: async (payload2, guard) => {
        if (!guard()) throw new Error("Synthetic peer denied");
        return { uuid: crypto.randomUUID(), streamUuid: input.stream, authorUuid: input.author, payload: payload2 };
      }
    });
    const request = { uuid: input.request, streamUuid: input.stream, authorUuid: input.requester, payload: {
      kind: "key_request",
      keyUuid: input.key,
      deviceId: input.recipientDevice,
      deviceName: "Synthetic native device",
      publicKey: { bytes: new Uint8Array(Buffer.from(input.publicKey, "base64")) }
    } };
    const response = await controller.grantKey(request);
    if (!response.ok) throw new Error("Synthetic peer grant unavailable");
    const encrypted = await provider.encryptMessage({ groupKey: key.value, plaintext: readPlaintext, context: { streamUuid: input.stream, keyUuid: input.key } });
    if (!encrypted.ok) throw new Error("Synthetic ciphertext unavailable");
    process.stdout.write(JSON.stringify({
      ok: true,
      readyForNativeSend: true,
      message: { uuid: response.value.uuid, author_uuid: input.author, payload: encodeChatE2eePayload(response.value.payload) },
      encrypted: { kind: "e2ee", key_uuid: input.key, content: Buffer.from(encrypted.value.bytes).toString("base64") }
    }) + "\n");
    const second = await iterator.next();
    if (second.done) throw new Error("Synthetic native envelope missing");
    const frame = second.value, payload = frame.payload;
    if (frame.kind !== "verify-native-send" || frame.stream !== input.stream || frame.topic !== input.targetTopic || !payload || Object.keys(payload).sort().join(",") !== "content,key_uuid,kind" || payload.kind !== "e2ee" || payload.key_uuid !== input.key || typeof payload.content !== "string" || payload.content.length > 213336 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(payload.content))
      throw new Error("Synthetic native envelope invalid");
    const envelope = Buffer.from(payload.content, "base64");
    if (envelope.length < 48 || envelope.toString("base64") !== payload.content) throw new Error("Synthetic native envelope invalid");
    const result = await provider.decryptMessage({
      groupKey: key.value,
      envelope: { bytes: new Uint8Array(envelope) },
      context: { streamUuid: input.stream, keyUuid: input.key }
    });
    if (!result.ok) throw new Error("Synthetic native envelope could not decrypt");
    cleartext = result.value;
    const matchesExpected = cleartext.length === expected.length && cleartext.every((byte, index) => byte === expected[index]);
    if (!(await iterator.next()).done) throw new Error("Synthetic peer extra frame");
    process.stdout.write(JSON.stringify({ ok: true, matchesExpected, plaintextBytes: cleartext.length, keyUuid: input.key }) + "\n");
  } finally {
    cleartext?.fill(0);
    expected.fill(0);
    readPlaintext.fill(0);
    key.value.bytes.fill(0);
    controller?.invalidate();
  }
}
async function verifyNativeKeyResponseMode(mixedOutcomes = false) {
  const maximumLineBytes = 512 * 1024;
  async function* frames() {
    let buffered = Buffer.alloc(0);
    for await (const chunk of process.stdin) {
      const bytes = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
      if (buffered.length + bytes.length > maximumLineBytes + 65536) throw new Error("Synthetic peer frame too large");
      buffered = Buffer.concat([buffered, bytes]);
      for (let end; (end = buffered.indexOf(10)) !== -1; ) {
        if (end === 0 || end > maximumLineBytes) throw new Error("Synthetic peer frame invalid");
        const line = buffered.subarray(0, end);
        buffered = buffered.subarray(end + 1);
        yield JSON.parse(line.toString("utf8"));
      }
      if (buffered.length > maximumLineBytes) throw new Error("Synthetic peer frame too large");
    }
    if (buffered.length !== 0) throw new Error("Synthetic peer frame incomplete");
  }
  const iterator = frames(), first = await iterator.next();
  if (first.done) throw new Error("Synthetic peer input missing");
  const input = first.value;
  if (typeof input.plaintext !== "string" || typeof input.seedNative !== "boolean" || typeof input.requestTopic !== "string" || typeof input.targetTopic !== "string" || input.sourceRecipient === input.requester || input.sourceDevice === input.nativeDevice)
    throw new Error("Synthetic independent recipient input invalid");
  const provider = createRealChatCryptoProvider();
  const key = await provider.createGroupKey();
  if (!key.ok) throw new Error("Synthetic source key unavailable");
  const plaintext = new TextEncoder().encode(input.plaintext);
  const pending = /* @__PURE__ */ new Map(), completions = /* @__PURE__ */ new Map(), pairs = [];
  let recipientKey = null, issuer, recipient;
  let decrypted;
  try {
    const owner = { accountId: "synthetic", instanceId: "synthetic", organizationId: "synthetic", projectId: input.project, userUuid: input.author };
    const scope = { owner, deviceId: input.senderDevice, streamUuid: input.stream, keyUuid: input.key, runtimeGeneration: 1 };
    issuer = new ChatE2eeController({
      scope,
      device: { deviceId: scope.deviceId, deviceName: "Synthetic JS issuer" },
      provider,
      storage: { readChatKey: async () => ({ groupKey: key.value }), readChatKeyDeletionRevision: async () => 0 },
      getCurrentScope: () => scope,
      getRights: () => ({ canRequest: true, canSend: true, canGrant: true, canReject: true, canRotate: false }),
      isIssuerAllowed: () => true,
      isAnnouncerAllowed: () => true,
      isRequesterAllowed: () => true,
      sendPayload: async (payload2, guard) => {
        if (!guard()) throw new Error("Synthetic source issuer denied");
        return { uuid: mixedOutcomes ? createMessengerPlacementUuid(input.targetTopic, crypto.randomUUID()) : crypto.randomUUID(), streamUuid: input.stream, authorUuid: input.author, payload: payload2 };
      }
    });
    let nativeGrant = null;
    if (input.seedNative) {
      const request2 = { uuid: input.request, streamUuid: input.stream, authorUuid: input.requester, payload: {
        kind: "key_request",
        keyUuid: input.key,
        deviceId: input.nativeDevice,
        deviceName: "Synthetic native device",
        publicKey: { bytes: new Uint8Array(Buffer.from(input.publicKey, "base64")) }
      } };
      const granted = await issuer.grantKey(request2);
      if (!granted.ok) throw new Error("Synthetic native seed grant failed");
      nativeGrant = { uuid: granted.value.uuid, author_uuid: input.author, payload: encodeChatE2eePayload(granted.value.payload) };
    }
    const probe = await provider.encryptMessage({ groupKey: key.value, plaintext, context: { streamUuid: input.stream, keyUuid: input.key } });
    if (!probe.ok) throw new Error("Synthetic source probe unavailable");
    const recipientScope = { ...scope, owner: { ...owner, userUuid: input.sourceRecipient }, deviceId: input.sourceDevice };
    const storage = {
      readChatKey: async () => recipientKey,
      readChatKeyDeletionRevision: async () => 0,
      listPendingRequests: async () => [...pending.values()],
      writePendingRequest: async (_address, item, options) => {
        if (options?.isCurrent?.() === false || options?.expectedDeletionRevision !== 0) return false;
        pending.set(item.localRequestId, item);
        pairs.push(item);
        return true;
      },
      bindPendingRequest: async (_address, id, uuid, options) => {
        if (options?.isCurrent?.() === false || !pending.has(id)) return false;
        pending.set(id, { ...pending.get(id), requestMessageUuid: uuid });
        return true;
      },
      readRequestCompletion: async (_address, id) => completions.get(id) ?? null,
      readRequestCompletionByMessageUuid: async (_address, uuid) => [...completions.values()].find((item) => item.requestMessageUuid === uuid) ?? null,
      completeGrant: async (_address, grant, options) => {
        const saved2 = pending.get(grant.localRequestId);
        if (options?.isCurrent?.() === false || !saved2 || saved2.requestMessageUuid !== grant.requestMessageUuid || completions.has(grant.localRequestId)) return false;
        recipientKey = { groupKey: { bytes: grant.groupKey.bytes.slice() } };
        pending.delete(grant.localRequestId);
        completions.set(grant.localRequestId, {
          localRequestId: grant.localRequestId,
          requestMessageUuid: grant.requestMessageUuid,
          responseMessageUuid: grant.responseMessageUuid,
          outcome: "granted"
        });
        return true;
      },
      completeReject: async (_address, reject, options) => {
        const saved2 = pending.get(reject.localRequestId);
        if (options?.isCurrent?.() === false || !saved2 || saved2.requestMessageUuid !== reject.requestMessageUuid || completions.has(reject.localRequestId)) return false;
        pending.delete(reject.localRequestId);
        completions.set(reject.localRequestId, { ...reject, outcome: "rejected" });
        return true;
      }
    };
    recipient = new ChatE2eeController({
      scope: recipientScope,
      device: { deviceId: input.sourceDevice, deviceName: "Synthetic independent recipient" },
      provider,
      storage,
      getCurrentScope: () => recipientScope,
      getRights: () => ({ canRequest: true, canSend: true, canGrant: false, canReject: false, canRotate: false }),
      isIssuerAllowed: (author) => author === input.requester && input.nativeIssuerAllowed === true,
      isAnnouncerAllowed: () => false,
      isRequesterAllowed: () => true,
      sendPayload: async (payload2, guard, canonical) => {
        if (!guard() || payload2.kind !== "key_request" || !canonical) throw new Error("Synthetic recipient request denied");
        return { uuid: createMessengerPlacementUuid(input.requestTopic, canonical), streamUuid: input.stream, authorUuid: input.sourceRecipient, payload: payload2 };
      }
    });
    const loaded = await recipient.loadKey();
    if (!loaded.ok) throw new Error("Synthetic recipient load failed");
    const requested = await recipient.requestKey();
    if (!requested.ok || requested.value.payload.kind !== "key_request") throw new Error("Synthetic source recipient request failed");
    const request = requested.value;
    const saved = [...pending.values()][0];
    if (pending.size !== 1 || !saved || saved.requestMessageUuid !== request.uuid || recipientKey?.groupKey) throw new Error("Synthetic recipient pending identity invalid");
    let mixedGrant = null, mixedReject = null;
    if (mixedOutcomes) {
      const granted = await issuer.grantKey(request), rejected = await issuer.rejectKey(request);
      if (!granted.ok || !rejected.ok) throw new Error("Synthetic mixed outcome pair unavailable");
      mixedGrant = { uuid: granted.value.uuid, author_uuid: granted.value.authorUuid, payload: encodeChatE2eePayload(granted.value.payload) };
      mixedReject = { uuid: rejected.value.uuid, author_uuid: rejected.value.authorUuid, payload: encodeChatE2eePayload(rejected.value.payload) };
    }
    process.stdout.write(JSON.stringify({
      ok: true,
      readyForNativeKeyResponse: true,
      nativeGrant,
      encrypted: { kind: "e2ee", key_uuid: input.key, content: Buffer.from(probe.value.bytes).toString("base64") },
      request: { uuid: request.uuid, author_uuid: request.authorUuid, payload: encodeChatE2eePayload(request.payload) },
      recipientPending: pending.size,
      recipientHasKey: false,
      independentOwner: input.sourceRecipient,
      independentDevice: input.sourceDevice,
      ...mixedOutcomes ? { mixedOutcomePair: true, mixedGrant, mixedReject } : {}
    }) + "\n");
    const second = await iterator.next();
    if (second.done) throw new Error("Synthetic native response missing");
    const frame = second.value;
    if (frame.kind === "finish-without-response") {
      if (!(await iterator.next()).done) throw new Error("Synthetic peer extra frame");
      process.stdout.write(JSON.stringify({
        ok: true,
        recipientStillPending: pending.size === 1,
        recipientHasKey: recipientKey?.groupKey != null,
        completionCount: completions.size
      }) + "\n");
      return;
    }
    const response = frame.response, payload = response?.payload;
    if (frame.kind !== "verify-native-key-response" || !response || response.project_id !== input.project || response.stream_uuid !== input.stream || response.topic_uuid !== input.targetTopic || response.author_uuid !== input.requester || response.uuid !== createMessengerPlacementUuid(input.targetTopic, frame.canonical) || !payload || payload.kind !== "key_grant" && payload.kind !== "key_reject" || payload.key_uuid !== input.key || payload.request_message_uuid !== request.uuid || payload.sender_device_id !== input.nativeDevice || payload.recipient_device_id !== input.sourceDevice)
      throw new Error("Synthetic native response identity invalid");
    const decoded = decodeChatE2eePayload(payload);
    if (!decoded.ok) throw new Error("Synthetic native response codec invalid");
    const ingested = await recipient.ingestKeyMessage({ uuid: response.uuid, streamUuid: input.stream, authorUuid: response.author_uuid, payload: decoded.value });
    if (!ingested.ok) throw new Error("Synthetic source recipient rejected native response");
    const completed = completions.get(saved.localRequestId);
    let matchesExpected = false, groupMatchesIssuer = false, nativeProbeVerified = false, matchesNativeProbe = false;
    if (frame.nativeProbe !== void 0) {
      if (input.seedNative || mixedOutcomes || payload.kind !== "key_grant" || !recipientKey?.groupKey)
        throw new Error("Synthetic native public probe context invalid");
      const publicProbe = frame.nativeProbe;
      if (!publicProbe || typeof publicProbe !== "object" || Object.keys(publicProbe).sort().join(",") !== "content,key_uuid,kind" || publicProbe.kind !== "e2ee" || publicProbe.key_uuid !== input.key || typeof publicProbe.content !== "string" || publicProbe.content.length > 4096) throw new Error("Synthetic native public probe invalid");
      const decodedProbe = decodeChatE2eePayload(publicProbe);
      if (!decodedProbe.ok || decodedProbe.value.kind !== "e2ee") throw new Error("Synthetic native public probe codec invalid");
      const read = await provider.decryptMessage({
        groupKey: recipientKey.groupKey,
        envelope: decodedProbe.value.content,
        context: { streamUuid: input.stream, keyUuid: input.key }
      });
      if (!read.ok) throw new Error("Synthetic native public probe could not decrypt");
      decrypted = read.value;
      nativeProbeVerified = true;
      matchesNativeProbe = decrypted.length === plaintext.length && decrypted.every((byte, index) => byte === plaintext[index]);
      groupMatchesIssuer = recipientKey.groupKey.bytes.length === key.value.bytes.length && recipientKey.groupKey.bytes.every((byte, index) => byte === key.value.bytes[index]);
    } else if (payload.kind === "key_grant") {
      if (!recipientKey?.groupKey) throw new Error("Synthetic recipient key not imported");
      groupMatchesIssuer = recipientKey.groupKey.bytes.length === key.value.bytes.length && recipientKey.groupKey.bytes.every((byte, index) => byte === key.value.bytes[index]);
      const read = await provider.decryptMessage({ groupKey: recipientKey.groupKey, envelope: probe.value, context: { streamUuid: input.stream, keyUuid: input.key } });
      if (!read.ok) throw new Error("Synthetic recipient could not decrypt");
      decrypted = read.value;
      matchesExpected = decrypted.length === plaintext.length && decrypted.every((byte, index) => byte === plaintext[index]);
    }
    if (!(await iterator.next()).done) throw new Error("Synthetic peer extra frame");
    process.stdout.write(JSON.stringify({
      ok: true,
      completionOutcome: completed?.outcome ?? null,
      completionRequest: completed?.requestMessageUuid ?? null,
      completionResponse: completed?.responseMessageUuid ?? null,
      recipientPending: pending.size,
      recipientHasKey: recipientKey?.groupKey != null,
      matchesExpected,
      groupMatchesIssuer,
      ...frame.nativeProbe !== void 0 ? { nativeProbeVerified, matchesNativeProbe } : {}
    }) + "\n");
  } finally {
    decrypted?.fill(0);
    plaintext.fill(0);
    recipientKey?.groupKey?.bytes.fill(0);
    key.value.bytes.fill(0);
    for (const item of pairs) {
      item.privateKey.fill(0);
      item.recovery?.previousGroupKey?.bytes.fill(0);
      item.recovery?.envelope?.bytes.fill(0);
    }
    pending.clear();
    completions.clear();
    recipient?.invalidate();
    issuer?.invalidate();
  }
}
if (process.argv.slice(2).includes("--mixed-outcomes-pair")) {
  await verifyNativeKeyResponseMode(true);
} else if (process.argv.slice(2).includes("--verify-native-key-response")) {
  await verifyNativeKeyResponseMode();
} else if (process.argv.slice(2).includes("--verify-native-send")) {
  await verifyNativeSendMode();
} else {
  const input = JSON.parse(fs.readFileSync(0, "utf8"));
  const provider = createRealChatCryptoProvider();
  const key = await provider.createGroupKey();
  if (!key.ok) throw new Error("Synthetic peer key unavailable");
  const owner = { accountId: "synthetic", instanceId: "synthetic", organizationId: "synthetic", projectId: input.project, userUuid: input.author };
  const scope = { owner, deviceId: input.senderDevice, streamUuid: input.stream, keyUuid: input.key, runtimeGeneration: 1 };
  const manager = input.role === "owner" || input.role === "administrator";
  const controller = new ChatE2eeController({
    scope,
    device: { deviceId: scope.deviceId, deviceName: "Synthetic JS issuer" },
    provider,
    storage: { readChatKey: async () => ({ groupKey: key.value }), readChatKeyDeletionRevision: async () => 0 },
    getCurrentScope: () => scope,
    getRights: () => ({ canRequest: true, canSend: true, canGrant: manager, canReject: manager, canRotate: input.role === "owner" }),
    isIssuerAllowed: () => manager,
    isAnnouncerAllowed: () => manager,
    isRequesterAllowed: () => true,
    sendPayload: async (payload, guard) => {
      if (!guard()) throw new Error("Synthetic peer denied");
      return { uuid: crypto.randomUUID(), streamUuid: input.stream, authorUuid: input.author, payload };
    }
  });
  const request = { uuid: input.request, streamUuid: input.stream, authorUuid: input.requester, payload: {
    kind: "key_request",
    keyUuid: input.key,
    deviceId: input.recipientDevice,
    deviceName: "Synthetic native device",
    publicKey: { bytes: new Uint8Array(Buffer.from(input.publicKey, "base64")) }
  } };
  const response = input.reject ? await controller.rejectKey(request) : await controller.grantKey(request);
  let output;
  if (!response.ok) output = { ok: false, reason: response.reason };
  else {
    const encrypted = await provider.encryptMessage({ groupKey: key.value, plaintext: new TextEncoder().encode(input.plaintext), context: { streamUuid: input.stream, keyUuid: input.key } });
    if (!encrypted.ok) throw new Error("Synthetic ciphertext unavailable");
    output = {
      ok: true,
      message: { uuid: response.value.uuid, author_uuid: input.author, payload: encodeChatE2eePayload(response.value.payload) },
      encrypted: { kind: "e2ee", key_uuid: input.key, content: Buffer.from(encrypted.value.bytes).toString("base64") }
    };
  }
  key.value.bytes.fill(0);
  controller.invalidate();
  process.stdout.write(JSON.stringify(output));
}
/*! Bundled license information:

@hpke/common/esm/src/curve/modular.js:
@hpke/common/esm/src/curve/montgomery.js:
@hpke/dhkem-x25519/esm/src/primitives/x25519.js:
  (*! noble-curves - MIT License (c) 2022 Paul Miller (paulmillr.com) *)

@noble/ciphers/utils.js:
  (*! noble-ciphers - MIT License (c) 2023 Paul Miller (paulmillr.com) *)
*/
