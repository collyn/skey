use bamboo_core::{Engine, InputMethod, Mode};

fn main() {
    println!("=== Per-key delta (Telex W): ww ===");
    let mut e = Engine::new(InputMethod::telex_w());
    
    for ch in "ww".chars() {
        let mut ins = String::new();
        let bs = e.process_key_delta_into(ch, Mode::Vietnamese, &mut ins);
        let out = e.output();
        println!("'{}': bs={}, inserted=\"{}\", output=\"{}\"", ch, bs, ins, out);
    }

    println!("\n=== Per-key delta: ddd (Telex W) ===");
    e.reset();
    for ch in "ddd".chars() {
        let mut ins = String::new();
        let bs = e.process_key_delta_into(ch, Mode::Vietnamese, &mut ins);
        let out = e.output();
        println!("'{}': bs={}, inserted=\"{}\", output=\"{}\"", ch, bs, ins, out);
    }

    println!("\n=== Per-key delta: ass (Telex W) ===");
    e.reset();
    for ch in "ass".chars() {
        let mut ins = String::new();
        let bs = e.process_key_delta_into(ch, Mode::Vietnamese, &mut ins);
        let out = e.output();
        println!("'{}': bs={}, inserted=\"{}\", output=\"{}\"", ch, bs, ins, out);
    }

    println!("\n=== Per-key delta: vieetj (Telex W) ===");
    e.reset();
    for ch in "vieetj".chars() {
        let mut ins = String::new();
        let bs = e.process_key_delta_into(ch, Mode::Vietnamese, &mut ins);
        let out = e.output();
        println!("'{}': bs={}, inserted=\"{}\", output=\"{}\"", ch, bs, ins, out);
    }
}
