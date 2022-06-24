use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn commas(strings: Option<Vec<String>>) -> Option<String> {
    match strings {
        None => None,
        Some(actual) => Some(actual.join(", ")),
    }
}
