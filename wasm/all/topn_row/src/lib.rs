use scylla_bindgen::scylla_bindgen;
use std::collections::BTreeSet;
#[scylla_bindgen]
fn topn_row(
    acc: (u32, BTreeSet<(u32, String, u32)>, u32),
    v: String,
) -> (u32, BTreeSet<(u32, String, u32)>, u32) {
    let mut acc = acc;
    acc.1.insert((v.len() as u32, v, acc.2));
    acc.2 += 1;
    while acc.1.len() > acc.0 as usize {
        let min = acc.1.iter().next().unwrap().clone();
        acc.1.remove(&min);
    }
    acc
}
