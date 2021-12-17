use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn top3(strings: Vec<String>) -> Vec<String> {
    strings.iter().take(3).cloned().collect()
}
