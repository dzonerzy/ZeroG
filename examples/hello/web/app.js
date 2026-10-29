// App.greet is native code (main.c), called synchronously: no await.
const form = document.getElementById("form");
const name = document.getElementById("name");
const message = document.getElementById("message");

function greet() {
    message.textContent = App.greet(name.value);
    console.log(message.textContent);
}

form.addEventListener("submit", event => {
    event.preventDefault();
    greet();
});

greet();

// The build's smoke test: quit once the greeting went through.
if (location.hash === "#smoke")
    requestAnimationFrame(() => App.quit());
