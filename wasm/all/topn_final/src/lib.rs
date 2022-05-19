use scylla_bindgen::scylla_bindgen;
use std::collections::BTreeSet;
#[scylla_bindgen]
fn topn_final(
    acc: (u32, BTreeSet<(u32, String, u32)>, u32)
) -> std::vec::Vec<String> {
    let mut res = std::vec::Vec::new();
    for (_, v, _) in acc.1.iter() {
        res.push(v.clone());
    }
    res
}
