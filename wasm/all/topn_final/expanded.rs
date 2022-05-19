#![feature(prelude_import)]
#[prelude_import]
use std::prelude::rust_2018::*;
#[macro_use]
extern crate std;
use scylla_bindgen::scylla_bindgen;
use std::collections::BTreeSet;
extern crate base;
use crate::base::*;
#[no_mangle]
pub static _scylla_abi: u32 = 2;
fn _scylla_internal_topn_final(
    acc: (u32, BTreeSet<(u32, String, u32)>, u32),
) -> std::vec::Vec<String> {
    let mut res = std::vec::Vec::new();
    for (_, v, _) in acc.1.iter() {
        res.push(v.clone());
    }
    res
}
#[no_mangle]
pub extern "C" fn topn_final(acc: u64) -> u64 {
    _scylla_internal_topn_final(<(u32, BTreeSet<(u32, String, u32)>, u32)>::from_cql(acc))
        .into_cql()
}
