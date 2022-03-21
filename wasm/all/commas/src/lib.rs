use scylla_bindgen::scylla_bindgen;
#[scylla_bindgen]
fn commas(strings: Vec<String>) -> String {
    let mut newstr = String::new();
    let mut it = strings.iter();
    if let Some(s) = it.next() {
        newstr.push_str(&s);
    }
    while let Some(s) = it.next() {
        newstr.push_str(", ");
        newstr.push_str(&s);
    }
    newstr
}
