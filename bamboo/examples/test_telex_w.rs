use bamboo_core::{Engine, InputMethod, Config, Mode};

fn main() {
    // Simulate the fix: full re-creation instead of reset()
    let mut e = Engine::new(InputMethod::telex());
    
    // Process "Linux"
    let out = e.process("Linux", Mode::Vietnamese);
    println!("process('Linux') → '{}'", out);
    
    // Full re-creation (like our fixed skey_engine_reset)
    let im = e.input_method().clone();
    let cfg = e.config();
    e = Engine::with_config(im, cfg);
    
    // Process "M"
    let out = e.process("M", Mode::Vietnamese);
    println!("After re-create + process('M') → '{}'", out);
    
    // Full re-creation again
    let im = e.input_method().clone();
    let cfg = e.config();
    e = Engine::with_config(im, cfg);
    
    // Process "Mi"
    let out = e.process("Mi", Mode::Vietnamese);
    println!("After re-create + process('Mi') → '{}'", out);
    
    // Test more words
    let words = vec!["Mint", "telegram", "google", "facebook", "Linux"];
    for w in &words {
        let im = e.input_method().clone();
        let cfg = e.config();
        e = Engine::with_config(im, cfg);
        let out = e.process(w, Mode::Vietnamese);
        let valid = e.is_valid(false);
        println!("process('{}') → '{}' valid={}", w, out, valid);
    }
}
