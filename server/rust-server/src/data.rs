pub static DEFAULT_AVATAR_PNG: &[u8] =
<<<<<<< HEAD
    include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../data/avatar.png"));
=======
    include_bytes!(r"C:\Users\ipxo\Downloads\fart(1)\server\data\avatar.png");
>>>>>>> 1ea974db663c59b4548e1e9ee4db9a452ebe92a2

pub static SEED_MODULE_BIN: &[u8] =
    include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../data/module.bin"));

pub static KEY_BIN: &[u8] = include_bytes!(concat!(env!("CARGO_MANIFEST_DIR"), "/../data/key.bin"));

const _: () = assert!(SEED_MODULE_BIN.len() == 385_360);
const _: () = assert!(KEY_BIN.len() == 80);
