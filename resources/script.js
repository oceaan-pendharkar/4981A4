// Select elements
const button = document.getElementById('dance-button');
const imageContainer = document.getElementById('image-container');

// Event listener for button click
button.addEventListener('click', () => {
    // Clear the current content of the image container
    imageContainer.innerHTML = '';

    for (let i = 0; i < 3; i++) {
        const img = document.createElement('img');
        img.src = 'dancecat.gif';
        img.alt = 'Dancing Cat';
        img.style.margin = '10px';
        img.style.width = '200px';
        imageContainer.appendChild(img);
    }

    // Hide the button after it's clicked
    button.style.display = 'none';
});
