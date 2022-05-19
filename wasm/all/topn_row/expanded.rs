#![feature(prelude_import)]
#[prelude_import]
use std::prelude::rust_2018::*;
#[macro_use]
extern crate std;
use scylla_bindgen::scylla_bindgen;
extern crate base;
use crate::base::*;
#[no_mangle]
pub static _scylla_abi: u32 = 1;
fn _scylla_internal_top3(
    strings: Option<std::collections::BTreeSet<String>>,
    topn: Option<u16>,
) -> Vec<String> {
    match strings {
        None => [String::from("null")].to_vec(),
        Some(actuals) => match topn {
            None => [String::from("null2")].to_vec(),
            Some(n) => actuals.into_iter().take(n as usize).collect(),
        },
    }
}
#[no_mangle]
pub extern "C" fn top3(strings: u64, topn: u64) -> u64 {
    _scylla_internal_top3(
        <Option<std::collections::BTreeSet<String>>>::from_cql(strings),
        <Option<u16>>::from_cql(topn),
    )
    .into_cql()
}
