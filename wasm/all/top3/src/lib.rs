use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn top3(strings: Option<std::collections::BTreeSet<String>>) -> Vec<String> {
    // handle None
    strings.iter().take(3).cloned().collect()
}
