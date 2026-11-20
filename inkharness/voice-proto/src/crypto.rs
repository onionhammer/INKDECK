//! Protocol 2's encryption (docs/voice-protocol.md#encryption): a Noise `NNpsk0` handshake
//! keyed from the shared token, then every frame sealed with AES-256-GCM under a fresh key per
//! direction. AES-GCM rather than ChaCha20-Poly1305 because the board's mbedTLS has it (on the
//! ESP32-S3's AES unit) and is already in its firmware; ChaChaPoly would mean another library.
//!
//! The handshake carries the hellos: the device's in message 1, the daemon's answer in
//! message 2, so encryption adds no round trip. The token never crosses the network; a
//! side without it can't open message 1 (the daemon) or message 2 (the device). Both sides
//! make a new ephemeral key per connection, so a token stolen later doesn't open recordings
//! of earlier sessions (apart from message 1, which holds only the device's hello).

use std::sync::Arc;

use snow::{HandshakeState, StatelessTransportState};

use crate::{
    ClientMsg, Frame, MAX_PAYLOAD, MAX_SEALED_PAYLOAD, ProtoError, SEAL_OVERHEAD, ServerMsg,
    TYPE_AUDIO, TYPE_CONTROL,
};

pub const NOISE: &str = "Noise_NNpsk0_25519_AESGCM_SHA256";
/// Mixed into the handshake, so it can't be taken for another protocol's.
pub const PROLOGUE: &[u8] = b"inkharness voice 2";
/// HKDF's salt for the key: binds it to this use of the token.
const PSK_SALT: &[u8] = b"inkharness voice psk";
const DH_LEN: usize = 32;
const TAG_LEN: usize = 16;

/// The key both sides derive from the token: HKDF-SHA256 with `PSK_SALT`, no info, 32 bytes.
/// An empty token gives a key too: the connection is still encrypted, but anyone can open one.
pub fn psk(token: &str) -> [u8; 32] {
    let mut out = [0u8; 32];
    hkdf::Hkdf::<sha2::Sha256>::new(Some(PSK_SALT), token.as_bytes())
        .expand(&[], &mut out)
        .expect("32 bytes is a valid HKDF-SHA256 length");
    out
}

fn handshake(psk: &[u8; 32], initiator: bool) -> HandshakeState {
    let builder = snow::Builder::new(NOISE.parse().expect("a valid Noise name"))
        .prologue(PROLOGUE)
        .and_then(|b| b.psk(0, psk))
        .expect("psk 0 is valid for NNpsk0");
    if initiator {
        builder.build_initiator()
    } else {
        builder.build_responder()
    }
    .expect("the default resolver has 25519, AESGCM and SHA256")
}

/// The device's side of the handshake.
pub struct Initiator(HandshakeState);

impl Initiator {
    pub fn new(psk: &[u8; 32]) -> Self {
        Initiator(handshake(psk, true))
    }

    /// Message 1: the device's hello.
    pub fn hello(&mut self, hello: &ClientMsg) -> Result<Frame, ProtoError> {
        let json = serde_json::to_vec(hello)?;
        let len = DH_LEN + json.len() + TAG_LEN;
        if len > MAX_PAYLOAD {
            return Err(ProtoError::TooLong(len));
        }
        let mut msg = vec![0u8; len];
        let n = self.0.write_message(&json, &mut msg)?;
        msg.truncate(n);
        Ok(Frame::Handshake(msg))
    }

    /// Opens message 2: the daemon's answer to the hello (a control message), and the keys
    /// for the rest of the connection. `ProtoError::Handshake` if it wasn't made with this
    /// token.
    pub fn finish(mut self, msg: &[u8]) -> Result<(String, Keys), ProtoError> {
        let mut payload = vec![0u8; msg.len()];
        let n = self
            .0
            .read_message(msg, &mut payload)
            .map_err(|_| ProtoError::Handshake)?;
        payload.truncate(n);
        let json = String::from_utf8(payload).map_err(|_| ProtoError::Utf8)?;
        Ok((json, Keys::new(self.0)?))
    }
}

/// The daemon's side of the handshake.
pub struct Responder(HandshakeState);

impl Responder {
    pub fn new(psk: &[u8; 32]) -> Self {
        Responder(handshake(psk, false))
    }

    /// Opens message 1: the device's hello (a control message). `ProtoError::Handshake` if
    /// the device's token is a different one.
    pub fn hello(&mut self, msg: &[u8]) -> Result<String, ProtoError> {
        let mut payload = vec![0u8; msg.len()];
        let n = self
            .0
            .read_message(msg, &mut payload)
            .map_err(|_| ProtoError::Handshake)?;
        payload.truncate(n);
        String::from_utf8(payload).map_err(|_| ProtoError::Utf8)
    }

    /// Message 2: the answer to the hello (the daemon's hello, or an error), and the keys for
    /// the rest of the connection.
    pub fn answer(mut self, answer: &ServerMsg) -> Result<(Frame, Keys), ProtoError> {
        let json = serde_json::to_vec(answer)?;
        let len = DH_LEN + json.len() + TAG_LEN;
        if len > MAX_PAYLOAD {
            return Err(ProtoError::TooLong(len));
        }
        let mut msg = vec![0u8; len];
        let n = self.0.write_message(&json, &mut msg)?;
        msg.truncate(n);
        Ok((Frame::Handshake(msg), Keys::new(self.0)?))
    }
}

/// The two keys a finished handshake gives, one per direction.
pub struct Keys(Arc<StatelessTransportState>);

impl Keys {
    fn new(handshake: HandshakeState) -> Result<Keys, ProtoError> {
        Ok(Keys(Arc::new(handshake.into_stateless_transport_mode()?)))
    }

    /// What seals the frames this side sends, and what opens the ones it receives: one for
    /// each half of the connection.
    pub fn split(self) -> (Sealer, Opener) {
        (
            Sealer {
                state: self.0.clone(),
                nonce: 0,
            },
            Opener {
                state: self.0,
                nonce: 0,
            },
        )
    }
}

/// Seals outgoing frames. The nonce counts them, so the other side opens them only in order.
pub struct Sealer {
    state: Arc<StatelessTransportState>,
    nonce: u64,
}

impl Sealer {
    /// A sealed frame holding `frame` (control or audio): its type byte and payload,
    /// encrypted, then the tag.
    pub fn seal(&mut self, frame: &Frame) -> Result<Frame, ProtoError> {
        let (kind, payload) = frame.parts();
        if kind != TYPE_CONTROL && kind != TYPE_AUDIO {
            return Err(ProtoError::NotSealable);
        }
        if payload.len() > MAX_SEALED_PAYLOAD {
            return Err(ProtoError::TooLong(payload.len()));
        }
        let mut plain = Vec::with_capacity(1 + payload.len());
        plain.push(kind);
        plain.extend_from_slice(payload);
        let mut sealed = vec![0u8; plain.len() + TAG_LEN];
        let n = self.state.write_message(self.nonce, &plain, &mut sealed)?;
        self.nonce += 1;
        sealed.truncate(n);
        Ok(Frame::Sealed(sealed))
    }
}

/// Opens incoming sealed frames, in the order they were sealed.
pub struct Opener {
    state: Arc<StatelessTransportState>,
    nonce: u64,
}

impl Opener {
    /// The frame inside, or None if it holds a type this version doesn't know.
    /// `ProtoError::Open` if it was changed, replayed, reordered or sealed with another key:
    /// the connection can't go on after that.
    pub fn open(&mut self, sealed: &[u8]) -> Result<Option<Frame>, ProtoError> {
        if sealed.len() < SEAL_OVERHEAD {
            return Err(ProtoError::Open);
        }
        let mut plain = vec![0u8; sealed.len()];
        let n = self
            .state
            .read_message(self.nonce, sealed, &mut plain)
            .map_err(|_| ProtoError::Open)?;
        self.nonce += 1;
        plain.truncate(n);
        let payload = plain.split_off(1);
        match plain[0] {
            kind @ (TYPE_CONTROL | TYPE_AUDIO) => Frame::from_parts(kind, payload),
            _ => Ok(None),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hello() -> ClientMsg {
        ClientMsg::Hello {
            proto: crate::PROTO_VERSION,
            device: "test".into(),
            token: String::new(),
            mic_rate: 16000,
            tools: vec![],
        }
    }

    fn server_hello() -> ServerMsg {
        ServerMsg::Hello {
            proto: crate::PROTO_VERSION,
            session: "s1".into(),
            agent: None,
        }
    }

    /// One side's sending and receiving halves.
    type Halves = (Sealer, Opener);

    /// Both sides' halves after a handshake with these tokens.
    fn connect(device: &str, daemon: &str) -> Result<(Halves, Halves), ProtoError> {
        let mut initiator = Initiator::new(&psk(device));
        let Frame::Handshake(msg1) = initiator.hello(&hello())? else {
            unreachable!()
        };
        let mut responder = Responder::new(&psk(daemon));
        let json = responder.hello(&msg1)?;
        assert_eq!(serde_json::from_str::<ClientMsg>(&json).unwrap(), hello());
        let (Frame::Handshake(msg2), daemon_keys) = responder.answer(&server_hello())? else {
            unreachable!()
        };
        let (json, device_keys) = initiator.finish(&msg2)?;
        assert_eq!(
            serde_json::from_str::<ServerMsg>(&json).unwrap(),
            server_hello()
        );
        Ok((device_keys.split(), daemon_keys.split()))
    }

    #[test]
    fn psk_is_hkdf_sha256() {
        // HKDF-SHA256(salt "inkharness voice psk", ikm "correct horse", info "", 32), as
        // Python's hmac module computes it.
        assert_eq!(
            hex(&psk("correct horse")),
            "bc8ff2af4e1c69023a2660258dcf472227e33300b56bf39e2c48088d195d3db6"
        );
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    #[test]
    fn frames_go_both_ways_sealed() {
        let ((mut dev_seal, mut dev_open), (mut dmn_seal, mut dmn_open)) =
            connect("t", "t").unwrap();
        for i in 0..3u8 {
            let up = Frame::Audio(vec![i; 1024]);
            let Frame::Sealed(sealed) = dev_seal.seal(&up).unwrap() else {
                unreachable!()
            };
            assert_eq!(sealed.len(), 1024 + SEAL_OVERHEAD);
            assert!(
                !sealed.windows(16).any(|w| w == [i; 16]),
                "the audio shows through"
            );
            assert_eq!(dmn_open.open(&sealed).unwrap(), Some(up));

            let down = Frame::control(&ServerMsg::ReplyEnd).unwrap();
            let Frame::Sealed(sealed) = dmn_seal.seal(&down).unwrap() else {
                unreachable!()
            };
            assert_eq!(dev_open.open(&sealed).unwrap(), Some(down));
        }
    }

    #[test]
    fn a_different_token_fails_at_the_daemon() {
        let mut initiator = Initiator::new(&psk("board"));
        let Frame::Handshake(msg1) = initiator.hello(&hello()).unwrap() else {
            unreachable!()
        };
        let mut responder = Responder::new(&psk("daemon"));
        assert!(matches!(responder.hello(&msg1), Err(ProtoError::Handshake)));
        // No token at all is a token too.
        assert!(matches!(connect("", "daemon"), Err(ProtoError::Handshake)));
        assert!(connect("", "").is_ok());
    }

    #[test]
    fn a_forged_answer_fails_at_the_device() {
        let mut initiator = Initiator::new(&psk("t"));
        let Frame::Handshake(msg1) = initiator.hello(&hello()).unwrap() else {
            unreachable!()
        };
        // Someone without the token answers: they can't read message 1, so they make up
        // message 2 with their own key.
        let mut impostor = Responder::new(&psk("guess"));
        assert!(impostor.hello(&msg1).is_err());
        let mut other = Initiator::new(&psk("guess"));
        let Frame::Handshake(other1) = other.hello(&hello()).unwrap() else {
            unreachable!()
        };
        let mut impostor = Responder::new(&psk("guess"));
        impostor.hello(&other1).unwrap();
        let (Frame::Handshake(msg2), _) = impostor.answer(&server_hello()).unwrap() else {
            unreachable!()
        };
        assert!(matches!(
            initiator.finish(&msg2),
            Err(ProtoError::Handshake)
        ));
    }

    #[test]
    fn changed_replayed_or_reordered_frames_fail() {
        let ((mut seal, _), (_, mut open)) = connect("t", "t").unwrap();
        let mut sealed: Vec<Vec<u8>> = (0..3u8)
            .map(|i| match seal.seal(&Frame::Audio(vec![i; 8])).unwrap() {
                Frame::Sealed(s) => s,
                _ => unreachable!(),
            })
            .collect();

        // Changed: any bit, header type byte included (it is inside the seal).
        let mut changed = sealed[0].clone();
        changed[0] ^= 1;
        assert!(matches!(open.open(&changed), Err(ProtoError::Open)));
        // A failed open leaves the counter where it was: the real frame still opens.
        assert_eq!(
            open.open(&sealed[0]).unwrap(),
            Some(Frame::Audio(vec![0; 8]))
        );
        // Replayed.
        assert!(matches!(open.open(&sealed[0]), Err(ProtoError::Open)));
        // Reordered.
        assert!(matches!(open.open(&sealed[2]), Err(ProtoError::Open)));
        assert_eq!(
            open.open(&sealed[1]).unwrap(),
            Some(Frame::Audio(vec![1; 8]))
        );
        // Cut short.
        sealed[2].truncate(SEAL_OVERHEAD - 1);
        assert!(matches!(open.open(&sealed[2]), Err(ProtoError::Open)));
    }

    #[test]
    fn unknown_sealed_types_are_skipped() {
        let ((seal, _), (_, mut open)) = connect("t", "t").unwrap();
        // Seal a type-9 frame by hand, as a later version might.
        let mut sealed = vec![0u8; 3 + TAG_LEN];
        let n = seal
            .state
            .write_message(0, &[9, 1, 2], &mut sealed)
            .unwrap();
        assert_eq!(open.open(&sealed[..n]).unwrap(), None);
        assert!(matches!(
            Sealer {
                state: seal.state.clone(),
                nonce: 1
            }
            .seal(&Frame::Sealed(vec![])),
            Err(ProtoError::NotSealable)
        ));
    }

    /// The handshake written out step by step from the Noise spec, as the board does it
    /// (src/voice_noise.cpp), checked against snow. If this passes, the steps are right.
    mod reference {
        use aes_gcm::Aes256Gcm;
        use aes_gcm::aead::consts::U12;
        use aes_gcm::aead::{Aead, KeyInit, Payload};
        use hmac::{Hmac, Mac};
        use sha2::{Digest, Sha256};

        use super::*;

        fn hmac(key: &[u8], parts: &[&[u8]]) -> [u8; 32] {
            let mut mac = <Hmac<Sha256> as hmac::KeyInit>::new_from_slice(key).unwrap();
            for part in parts {
                mac.update(part);
            }
            mac.finalize().into_bytes().into()
        }

        /// Noise's HKDF: two or three outputs from the chaining key and the input.
        fn hkdf(ck: &[u8; 32], ikm: &[u8]) -> ([u8; 32], [u8; 32], [u8; 32]) {
            let temp = hmac(ck, &[ikm]);
            let o1 = hmac(&temp, &[&[1]]);
            let o2 = hmac(&temp, &[&o1, &[2]]);
            let o3 = hmac(&temp, &[&o2, &[3]]);
            (o1, o2, o3)
        }

        fn nonce(n: u64) -> aes_gcm::Nonce<U12> {
            let mut bytes = [0u8; 12];
            bytes[4..].copy_from_slice(&n.to_be_bytes()); // AESGCM's counter is big-endian
            bytes.into()
        }

        struct Symmetric {
            ck: [u8; 32],
            h: [u8; 32],
            k: [u8; 32],
            n: u64,
        }

        impl Symmetric {
            fn mix_hash(&mut self, data: &[u8]) {
                self.h = Sha256::new()
                    .chain_update(self.h)
                    .chain_update(data)
                    .finalize()
                    .into();
            }
            fn mix_key(&mut self, ikm: &[u8]) {
                let (ck, k, _) = hkdf(&self.ck, ikm);
                (self.ck, self.k, self.n) = (ck, k, 0);
            }
            fn mix_key_and_hash(&mut self, ikm: &[u8]) {
                let (ck, h, k) = hkdf(&self.ck, ikm);
                self.ck = ck;
                self.mix_hash(&h);
                (self.k, self.n) = (k, 0);
            }
            fn encrypt_and_hash(&mut self, plain: &[u8]) -> Vec<u8> {
                let cipher = Aes256Gcm::new(&self.k.into());
                let out = cipher
                    .encrypt(
                        &nonce(self.n),
                        Payload {
                            msg: plain,
                            aad: &self.h,
                        },
                    )
                    .unwrap();
                self.n += 1;
                self.mix_hash(&out);
                out
            }
            fn decrypt_and_hash(&mut self, sealed: &[u8]) -> Option<Vec<u8>> {
                let cipher = Aes256Gcm::new(&self.k.into());
                let out = cipher
                    .decrypt(
                        &nonce(self.n),
                        Payload {
                            msg: sealed,
                            aad: &self.h,
                        },
                    )
                    .ok()?;
                self.n += 1;
                self.mix_hash(sealed);
                Some(out)
            }
        }

        #[test]
        fn the_boards_steps_match_snow() {
            let token_psk = psk("correct horse");
            // InitializeSymmetric: a name of up to 32 bytes is h itself, padded with zeros
            // (a longer one would be hashed). This one is exactly 32.
            assert_eq!(NOISE.len(), 32);
            let mut h = [0u8; 32];
            h[..NOISE.len()].copy_from_slice(NOISE.as_bytes());
            let mut s = Symmetric {
                ck: h,
                h,
                k: [0; 32],
                n: 0,
            };
            s.mix_hash(PROLOGUE);

            // Message 1: psk, e, then the hello.
            s.mix_key_and_hash(&token_psk);
            let e_private = [0x42u8; 32];
            let e_public = x25519_dalek::x25519(e_private, x25519_dalek::X25519_BASEPOINT_BYTES);
            s.mix_hash(&e_public);
            s.mix_key(&e_public); // psk patterns mix e into the key too
            let hello = br#"{"type":"hello","proto":2}"#;
            let mut msg1 = e_public.to_vec();
            msg1.extend(s.encrypt_and_hash(hello));

            let mut responder = Responder::new(&token_psk);
            assert_eq!(responder.hello(&msg1).unwrap().as_bytes(), hello);
            let (Frame::Handshake(msg2), keys) = responder.answer(&server_hello()).unwrap() else {
                unreachable!()
            };

            // Message 2: e, ee, then the daemon's answer.
            let re: [u8; 32] = msg2[..32].try_into().unwrap();
            s.mix_hash(&re);
            s.mix_key(&re);
            s.mix_key(&x25519_dalek::x25519(e_private, re));
            let answer = s.decrypt_and_hash(&msg2[32..]).expect("message 2 opens");
            assert_eq!(
                serde_json::from_slice::<ServerMsg>(&answer).unwrap(),
                server_hello()
            );

            // Split: the first key seals what the device sends, the second what it receives.
            let (send, recv, _) = hkdf(&s.ck, &[]);
            let (mut daemon_seal, mut daemon_open) = keys.split();
            let up = Aes256Gcm::new(&send.into())
                .encrypt(&nonce(0), &[2u8, 7, 7][..])
                .unwrap();
            assert_eq!(
                daemon_open.open(&up).unwrap(),
                Some(Frame::Audio(vec![7, 7]))
            );
            let Frame::Sealed(down) = daemon_seal.seal(&Frame::Audio(vec![9])).unwrap() else {
                unreachable!()
            };
            let plain = Aes256Gcm::new(&recv.into())
                .decrypt(&nonce(0), down.as_slice())
                .unwrap();
            assert_eq!(plain, [2, 9]);
        }
    }
}
